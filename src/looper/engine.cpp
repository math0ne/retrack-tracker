#include "looper/engine.h"

#include <cmath>
#include <cstring>

namespace looper {

namespace {

constexpr uint32_t kMinMasterFrames = kSampleRate / 20;   // 50 ms: shorter presses are a mistake
constexpr uint32_t kZeroChunkFrames = 16384;              // per service() call, ~64 KB

inline int16_t sat16(float v) {
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return int16_t(v);
}

// Grain window: sin^2 over the grain, so two grains half a grain apart sum to 1.
constexpr uint32_t kWinSize = 512;
float gWin[kWinSize];

inline float lerp(int16_t a, int16_t b, float fr) { return float(a) + (float(b) - float(a)) * fr; }

inline uint8_t peak8(int a, int b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    const int m = a > b ? a : b;
    return uint8_t(m >> 7);
}

// Waveform peak cell of frame i: [2 * cell] left, [2 * cell + 1] right. A cell
// restarts at its first frame, so an overdub pass replaces the old peaks.
inline void notePeak(uint8_t* peaks, uint32_t i, int16_t l, int16_t r) {
    uint8_t* c = peaks + size_t(i / kPeakFrames) * 2;
    if (i % kPeakFrames == 0) c[0] = c[1] = 0;
    const uint8_t pl = peak8(l, 0), pr = peak8(r, 0);
    if (pl > c[0]) c[0] = pl;
    if (pr > c[1]) c[1] = pr;
}

// Mono tracks: one peak byte per cell (the cell array holds twice as many cells).
inline void notePeakMono(uint8_t* peaks, uint32_t i, int16_t v) {
    uint8_t* c = peaks + i / kPeakFrames;
    if (i % kPeakFrames == 0) c[0] = 0;
    const uint8_t p = peak8(v, 0);
    if (p > c[0]) c[0] = p;
}

inline int16_t fold(InputCh ch, int16_t l, int16_t r) {
    switch (ch) {
        case InputCh::kMonoL: return l;
        case InputCh::kMonoR: return r;
        default: return int16_t((int32_t(l) + int32_t(r)) / 2);
    }
}

// Reverb lines, one set per slot, in internal SRAM (.bss), not the uncached
// SDRAM: twelve random reads and writes per sample.
float gReverbLines[kFxSlots][Reverb::kFloats];

}  // namespace

void Engine::init(const TrackMemory mem[kTracks], uint8_t* fxMem) {
    // FX 1 starts as a delay, FX 2 as a reverb; either can be any type.
    for (int s = 0; s < kFxSlots; ++s) {
        fx_[s].init(reinterpret_cast<int16_t*>(fxMem + size_t(s) * Delay::kBytes), gReverbLines[s]);
        retPeak_[s].store(0);
    }
    fx_[1].setType(FxType::kReverb);
    for (int i = 0; i < kPerfFxCount; ++i) perfParam_[i] = kPerfKnobs[i].def;
    for (uint32_t i = 0; i < kWinSize; ++i) {
        const float s = std::sin(3.14159265f * float(i) / float(kWinSize));
        gWin[i] = s * s;
    }
    for (int t = 0; t < kTracks; ++t) {
        mem_[t] = mem[t];
        trk_[t] = Track{};
        // The SDRAM holds garbage after power-up: zero every buffer once.
        trk_[t].written = mem[t].capacity;
        zeroTodo_[t].store(mem[t].capacity);
    }
    for (int c = 0; c < 2; ++c) {
        inPeak_[c].store(0);
        outPeak_[c].store(0);
    }
    publish();
}

void Engine::post(Cmd c, uint8_t track, int16_t arg, uint32_t a, uint32_t b) {
    const uint32_t head = qHead_.load(std::memory_order_relaxed);
    const uint32_t next = (head + 1) % kQueue;
    if (next == qTail_.load(std::memory_order_acquire)) return;   // full: drop (UI rate is far below this)
    queue_[head] = Msg{uint8_t(c), track, arg, a, b};
    qHead_.store(next, std::memory_order_release);
}

void Engine::service() {
    for (int t = 0; t < kTracks; ++t) {
        uint32_t todo = zeroTodo_[t].load(std::memory_order_acquire);
        if (!todo) continue;
        const uint32_t n = todo < kZeroChunkFrames ? todo : kZeroChunkFrames;
        const uint32_t from = todo - n;
        std::memset(mem_[t].frames + size_t(from) * 2, 0, size_t(n) * 2 * sizeof(int16_t));
        const uint32_t c0 = from / kPeakFrames, c1 = (todo + kPeakFrames - 1) / kPeakFrames;
        std::memset(mem_[t].peaks + size_t(c0) * 2, 0, size_t(c1 - c0) * 2);
        zeroTodo_[t].store(from, std::memory_order_release);
        return;   // one chunk per call keeps the main loop responsive
    }
    if (job_.state.load(std::memory_order_acquire) == 1) runJobChunk();
}

void Engine::runJobChunk() {
    // Main loop. Cut: move the frames after the selection up over it, then
    // recompute the peak cells from there to the new end. Erase: zero the
    // selection, then recompute its cells. One chunk per call.
    EditJob& j = job_;
    const TrackMemory& m = mem_[j.track];
    const uint32_t fb = j.mono ? 2u : 4u;   // bytes per frame
    uint8_t* base = reinterpret_cast<uint8_t*>(m.frames) + size_t(j.base) * fb;
    const bool cut = j.kind == kEditCut;
    const uint32_t work = cut ? j.len - j.b : j.b - j.a;
    if (j.done < work) {
        const uint32_t n = work - j.done < kZeroChunkFrames ? work - j.done : kZeroChunkFrames;
        if (cut) std::memmove(base + size_t(j.a + j.done) * fb, base + size_t(j.b + j.done) * fb, size_t(n) * fb);
        else std::memset(base + size_t(j.a + j.done) * fb, 0, size_t(n) * fb);
        j.done += n;
        return;
    }
    const uint32_t valid = cut ? j.len - (j.b - j.a) : j.len;   // loop frames that hold audio now
    const uint32_t p1 = cut ? valid : j.b;
    const uint32_t c0 = (j.base + j.a) / kPeakFrames, c1 = (j.base + p1 + kPeakFrames - 1) / kPeakFrames;
    const uint32_t lo = j.base, hi = j.base + valid;   // absolute frames with audio
    for (uint32_t step = 0; step < 64 && c0 + j.peaksDone < c1; ++step, ++j.peaksDone) {
        const uint32_t c = c0 + j.peaksDone;
        uint32_t f0 = c * kPeakFrames, f1 = f0 + kPeakFrames;
        if (f0 < lo) f0 = lo;
        if (f1 > hi) f1 = hi;
        uint8_t pl = 0, pr = 0;
        for (uint32_t f = f0; f < f1; ++f) {
            if (j.mono) {
                const uint8_t p = peak8(m.frames[f], 0);
                if (p > pl) pl = p;
            } else {
                const uint8_t a = peak8(m.frames[2 * f], 0), b = peak8(m.frames[2 * f + 1], 0);
                if (a > pl) pl = a;
                if (b > pr) pr = b;
            }
        }
        if (j.mono) m.peaks[c] = pl;
        else {
            m.peaks[2 * c] = pl;
            m.peaks[2 * c + 1] = pr;
        }
    }
    if (c0 + j.peaksDone >= c1) j.state.store(2, std::memory_order_release);
}

void Engine::finishJob() {
    // Audio thread: the job is done; a cut takes its new length now.
    EditJob& j = job_;
    Track& k = trk_[j.track];
    if (j.kind == kEditCut) {
        k.len = j.len - (j.b - j.a);
        k.offset16 = 0;
        k.busy = false;
        restartGrains(k, uint64_t(posOf(k)) << 16);
    }
    view_[j.track].peakVersion.fetch_add(1, std::memory_order_relaxed);
    j.state.store(0, std::memory_order_release);
}

bool Engine::allEmpty() const {
    for (const Track& k : trk_)
        if (k.state != TrackState::kEmpty && k.state != TrackState::kClearing) return false;
    return true;
}

uint64_t Engine::period16(const Track& k) const {
    return (uint64_t(k.len) * (modeInfo(k.mode).pingPong ? 2u : 1u)) << 16;
}

uint64_t Engine::phase16(const Track& k) const {
    const uint64_t P = period16(k);
    if (!P) return 0;
    // The clock's fraction too: at a changed tempo it moves in parts of frames.
    const int64_t time16 = modeInfo(k.mode).time16;
    const int64_t d = (int64_t(t_) - int64_t(k.anchor)) * time16 + ((int64_t(tAcc_ & 0xFFFFu) * time16) >> 16) +
                      int64_t(k.offset16 % P);
    int64_t r = d % int64_t(P);
    if (r < 0) r += int64_t(P);
    return uint64_t(r);
}

uint32_t Engine::posOfPhase(const Track& k, uint64_t ph) const {
    const ModeInfo& mi = modeInfo(k.mode);
    const uint32_t p = uint32_t(ph >> 16);
    if (mi.reverse) return k.len - 1 - p;
    if (mi.pingPong && p >= k.len) return 2 * k.len - 1 - p;
    return p;
}

uint64_t Engine::phaseOfPos(const Track& k, uint32_t pos, bool secondHalf) const {
    const ModeInfo& mi = modeInfo(k.mode);
    if (mi.reverse) return uint64_t(k.len - 1 - pos) << 16;
    if (mi.pingPong && secondHalf) return uint64_t(2 * k.len - 1 - pos) << 16;
    return uint64_t(pos) << 16;
}

void Engine::restartGrains(Track& k, uint64_t pos16) {
    // The second grain starts at its window peak, so the sound carries on at once.
    k.grain[0] = Grain{pos16, 0};
    k.grain[1] = Grain{pos16, kGrainFrames / 2};
}

void Engine::setMode(int t, PlayMode mode) {
    Track& k = trk_[t];
    if (!k.len) {   // empty or recording: the mode applies once there is a loop
        k.mode = mode;
        return;
    }
    // Keep the read position: the new phase function passes through it now.
    const uint64_t ph = phase16(k);
    const uint32_t pos = posOfPhase(k, ph);
    const bool second = modeInfo(k.mode).pingPong && (ph >> 16) >= k.len;
    k.mode = mode;
    const uint64_t P = period16(k);
    const uint64_t want = phaseOfPos(k, pos, second);
    k.offset16 = 0;
    k.offset16 = (want + P - phase16(k)) % P;
    restartGrains(k, uint64_t(pos) << 16);
    if (k.state == TrackState::kOverdubbing && !canDubNow(k)) k.state = TrackState::kPlaying;
}

float Engine::baseTempo() const {
    if (!L_) return float(bpm_);
    return float(kSampleRate) * 60.0f * float(beats_) / float(L_);
}

uint32_t Engine::barLen() const {
    // The master loop's bar once there is one; before that, the tempo's.
    if (L_) return L_;
    return uint32_t(uint64_t(kSampleRate) * 60u * beats_ / (bpm_ ? bpm_ : 120));
}

void Engine::startRecording(int t) {
    Track& k = trk_[t];
    const TrackOpts& o = opts_[t];
    if (L_ == 0) {
        // A second track pressed while the first take is still going closes
        // that take first, so the bar is set once.
        for (int x = 0; x < kTracks; ++x)
            if (x != t && trk_[x].state == TrackState::kRecording) finishNow(x);
            else if (x != t && trk_[x].state == TrackState::kCountIn) clearTrack(x);
    }
    const bool first = L_ == 0;
    k.mono = o.input != InputCh::kStereo;
    k.input = o.input;
    k.len = 0;
    k.offset16 = 0;
    k.written = 0;
    k.start = 0;   // a take fills its buffer from the start
    k.undo.has = false;
    if (o.start == StartMode::kSound) {
        // Armed: render() starts the take (and, with no loop yet, the bar clock)
        // when the input crosses the threshold.
        k.state = TrackState::kArmed;
        return;
    }
    if (first || !running_) startClockAlone(t);   // the other loops stay stopped
    const uint32_t bar = barLen(), beat = beatLen();
    k.stopAt = o.lenBeats ? uint32_t(o.lenBeats) * beat : 0;
    if (o.start == StartMode::kCountIn) {
        // One bar of click, then record from the bar line. With loops already
        // running, the next bar line at least half a bar away.
        uint32_t start = first ? bar : ((t_ + bar - 1) / bar) * bar;
        if (!first && start - t_ < bar / 2) start += bar;
        k.anchor = start;
        k.recStart = t_;
        k.state = TrackState::kCountIn;
    } else {
        beginTake(t, t_, nullptr, 0);
    }
}

void Engine::beginTake(int t, uint32_t tf, const int16_t* pre, uint32_t preFrames) {
    // The take starts at global frame tf. Buffer index 0 sits on the bar (or
    // beat) line before it, so the take stays on the grid; the frames before tf
    // stay silent (the buffer is zeroed) except the pre-roll just before tf.
    // Free takes (with no fixed length) are not on the grid: they start at tf,
    // or at the start of the pre-roll.
    Track& k = trk_[t];
    const TrackOpts& o = opts_[t];
    uint32_t unit = o.stop == StopQuant::kBar && !o.lenBeats ? barLen() : beatLen();
    if (o.stop == StopQuant::kFree && !o.lenBeats) unit = 0;
    k.anchor = unit ? (tf / unit) * unit : tf - (preFrames < tf ? preFrames : tf);
    k.written = tf - k.anchor;
    k.recStart = tf;
    k.stopAt = o.lenBeats ? uint32_t(o.lenBeats) * beatLen() : 0;
    const uint32_t n = preFrames < k.written ? preFrames : k.written;
    const TrackMemory& m = mem_[t];
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t dst = k.written - n + i, src = preFrames - n + i;
        const int16_t l = pre[2 * src], r = pre[2 * src + 1];
        if (k.mono) {
            const int16_t v = fold(k.input, l, r);
            m.frames[dst] = v;
            notePeakMono(m.peaks, dst, v);
        } else {
            m.frames[2 * dst] = l;
            m.frames[2 * dst + 1] = r;
            notePeak(m.peaks, dst, l, r);
        }
    }
    k.recPos16 = int64_t(k.written) << 16;
    k.prevL = n ? pre[2 * (preFrames - 1)] : 0;
    k.prevR = n ? pre[2 * (preFrames - 1) + 1] : 0;
    k.state = TrackState::kRecording;
}

void Engine::dubVari(int t, uint64_t ph, uint64_t step16, const int16_t* in, float fb) {
    // Overdub at a changed tempo (Forward only, so the phase is the loop frame):
    // the input resampled onto the buffer frames between the last input frame
    // and this one, each mixed into what is there.
    Track& k = trk_[t];
    const TrackMemory& m = mem_[t];
    const int16_t l = in ? in[0] : 0, r = in ? in[1] : 0;
    const bool mono = k.mono;
    int16_t* const fr = m.frames + (mono ? size_t(k.start) : size_t(k.start) * 2);
    const int64_t hi = int64_t(ph), lo = hi - int64_t(step16);
    for (int64_t x = (lo >> 16) + 1; x <= (hi >> 16); ++x) {
        const uint32_t i = uint32_t(((x % int64_t(k.len)) + int64_t(k.len)) % int64_t(k.len));
        const float a = step16 ? float(x * 65536 - lo) / float(step16) : 1.0f;
        const float vl = lerp(k.prevL, l, a), vr = lerp(k.prevR, r, a);
        if (mono) {
            const int16_t nv = sat16(float(fr[i]) * fb + float(fold(k.input, sat16(vl), sat16(vr))));
            fr[i] = nv;
            notePeakMono(m.peaks, k.start + i, nv);
        } else {
            const int16_t nl = sat16(float(fr[2 * i]) * fb + vl);
            const int16_t nr = sat16(float(fr[2 * i + 1]) * fb + vr);
            fr[2 * i] = nl;
            fr[2 * i + 1] = nr;
            notePeak(m.peaks, k.start + i, nl, nr);
        }
    }
    k.prevL = l;
    k.prevR = r;
}

void Engine::requestStop(int t) {
    Track& k = trk_[t];
    if (k.state == TrackState::kCountIn || k.state == TrackState::kArmed) {   // pressed again: cancel
        clearTrack(t);
        return;
    }
    const TrackOpts& o = opts_[t];
    const uint32_t n = k.written;
    if (o.stop == StopQuant::kFree) {
        finishRecording(t, n);
        return;
    }
    const uint32_t unit = o.stop == StopQuant::kBeat ? beatLen() : barLen();
    const uint32_t below = n / unit * unit;
    // Just past a line (within an eighth of a unit): a late press, stop on that line.
    if (below && n - below <= unit / kSegments) {
        finishRecording(t, below);
        return;
    }
    k.stopAt = below + unit;   // keep recording to the next line
}

void Engine::finishNow(int t) {
    Track& k = trk_[t];
    if (k.state == TrackState::kCountIn || k.state == TrackState::kArmed) {
        clearTrack(t);
        return;
    }
    const TrackOpts& o = opts_[t];
    const uint32_t n = k.written;
    if (o.stop == StopQuant::kFree && !o.lenBeats) {
        finishRecording(t, n);
        return;
    }
    const uint32_t unit = o.stop == StopQuant::kBar && !o.lenBeats ? barLen() : beatLen();
    uint32_t len = (n + unit - 1) / unit * unit;   // the rest of the unit stays silent
    if (len > capOf(t)) len = capOf(t) / unit * unit;
    finishRecording(t, len ? len : n);
}

void Engine::finishRecording(int t, uint32_t len) {
    Track& k = trk_[t];
    const TrackOpts& o = opts_[t];
    const uint32_t n = k.written;
    if (L_ == 0) {
        if (len < kMinMasterFrames) {
            clearTrack(t);
            return;
        }
        // A free first take is one bar long and sets the tempo; on bar, on beat
        // or fixed-length first takes keep the tempo's bar.
        L_ = (o.stop == StopQuant::kFree && !o.lenBeats) ? len : barLen();
    }
    if (len > capOf(t)) len = capOf(t);
    k.len = len;
    k.stopAt = 0;
    // Frames past len (a late press) were written too: the next clear zeroes them.
    k.written = n > len ? n : len;
    switch (o.after) {
        case AfterRec::kPlay: k.state = TrackState::kPlaying; break;
        case AfterRec::kOverdub:
            k.state = canDubNow(k) ? TrackState::kOverdubbing : TrackState::kPlaying;
            break;
        case AfterRec::kStop: k.state = TrackState::kStopped; break;
    }
    restartGrains(k, uint64_t(posOf(k)) << 16);
}

void Engine::clearTrack(int t) {
    Track& k = trk_[t];
    const uint8_t vol = k.volume;
    // Zeroing counts stereo frames (4 bytes); a mono track's frames are 2 bytes.
    const uint32_t dirty = k.mono ? (k.written + 1) / 2 : k.written;
    k = Track{};
    k.volume = vol;
    k.written = dirty;
    if (dirty) {
        k.state = TrackState::kClearing;
        zeroTodo_[t].store(dirty, std::memory_order_release);
    } else {
        k.state = TrackState::kEmpty;
    }
    if (allEmpty()) {
        L_ = 0;
        setClock(0);
        speed16_ = 65536;   // no loop: the tempo is the set tempo again
        running_ = false;
    }
}

void Engine::apply(const Msg& m) {
    const int t = m.track < kTracks ? m.track : 0;
    Track& k = trk_[t];
    switch (Cmd(m.cmd)) {
        case kRecord:
            switch (k.state) {
                case TrackState::kEmpty: startRecording(t); break;
                case TrackState::kCountIn:
                case TrackState::kArmed:
                case TrackState::kRecording: requestStop(t); break;
                case TrackState::kPlaying:
                    if (!canDubNow(k)) break;   // the app tells the user why
                    if (!running_) startClockAlone(t);
                    k.state = TrackState::kOverdubbing;
                    break;
                case TrackState::kStopped:   // start from the top on the next bar, overdubbing
                    if (canDubNow(k)) queueLaunch(t, true);
                    break;
                case TrackState::kQueued: k.launchDub = canDubNow(k); break;
                case TrackState::kOverdubbing: k.state = TrackState::kPlaying; break;
                case TrackState::kClearing: break;
            }
            break;
        case kPlayStop:
            switch (k.state) {
                case TrackState::kCountIn:
                case TrackState::kArmed:
                case TrackState::kRecording: requestStop(t); break;
                case TrackState::kPlaying:
                case TrackState::kOverdubbing:   // stop: back to its start (shown there)
                    k.state = TrackState::kStopped;
                    break;
                case TrackState::kStopped: queueLaunch(t, false); break;
                case TrackState::kQueued: k.state = TrackState::kStopped; break;   // cancel
                default: break;
            }
            break;
        case kMute: k.muted = !k.muted; break;
        case kSetMute: k.muted = m.arg != 0; break;
        case kSetBar:
            if (allEmpty() && m.a) L_ = m.a;
            break;
        case kInstall:
            // A loop the app has read into this empty track (its peaks too):
            // from its start, its bar phase kept, playing or stopped.
            if (k.state == TrackState::kEmpty && m.a && m.a <= (m.arg & 1 ? 2 * mem_[t].capacity : mem_[t].capacity)) {
                k.mono = (m.arg & 1) != 0;
                k.len = m.a;
                k.written = m.a;
                k.start = 0;
                k.anchor = m.b;
                k.offset16 = 0;
                k.undo.has = false;
                k.state = (m.arg & 2) ? TrackState::kStopped : TrackState::kPlaying;
                restartGrains(k, uint64_t(posOf(k)) << 16);
                view_[t].peakVersion.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        case kClear:
            if (job_.state.load(std::memory_order_acquire) && job_.track == t) break;   // an edit is running on it
            clearTrack(t);
            break;
        case kEditTrim:
            // Keep [a, b): the loop starts later in its buffer, nothing moves.
            // Its anchor moves by the same amount, so the kept part stays where
            // it was on the bar grid.
            if (k.len && !k.busy && m.b > m.a && m.b <= k.len && k.state != TrackState::kRecording) {
                k.undo.has = true;
                k.undo.start = k.start;
                k.undo.len = k.len;
                k.undo.anchor = k.anchor;
                k.start += m.a;
                k.anchor += m.a;
                k.len = m.b - m.a;
                k.offset16 = 0;
                restartGrains(k, uint64_t(posOf(k)) << 16);
                view_[t].peakVersion.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        case kEditUndo:
            if (k.undo.has && !k.busy) {
                k.start = k.undo.start;
                k.len = k.undo.len;
                k.anchor = k.undo.anchor;
                k.offset16 = 0;
                k.undo.has = false;
                restartGrains(k, uint64_t(posOf(k)) << 16);
                view_[t].peakVersion.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        case kEditCut:
        case kEditErase: {
            // A main-loop job; a cut leaves at least one frame and silences the
            // track while the audio moves.
            const bool cut = m.cmd == kEditCut;
            if (!k.len || k.busy || job_.state.load(std::memory_order_acquire) || m.b <= m.a || m.b > k.len) break;
            if (cut && m.b - m.a >= k.len) break;
            job_.track = uint8_t(t);
            job_.kind = m.cmd;
            job_.mono = k.mono;
            job_.base = k.start;
            job_.len = k.len;
            job_.a = m.a;
            job_.b = m.b;
            job_.done = job_.peaksDone = 0;
            if (cut) {
                k.busy = true;
                k.undo.has = false;   // the audio moves: an older trim cannot come back
            }
            job_.state.store(1, std::memory_order_release);
            break;
        }
        case kJump:
            // Move the read position to the same place inside eighth `arg`, in
            // whatever direction the mode is reading.
            if (k.len >= kSegments && m.arg >= 0 && m.arg < kSegments) {
                const uint32_t seg = k.len / kSegments;
                const uint64_t ph = phase16(k);
                const uint32_t pos = posOfPhase(k, ph);
                const bool second = modeInfo(k.mode).pingPong && (ph >> 16) >= k.len;
                uint32_t cur = pos / seg;
                if (cur >= uint32_t(kSegments)) cur = kSegments - 1;
                int64_t np = (int64_t(pos) + (int64_t(m.arg) - int64_t(cur)) * seg) % int64_t(k.len);
                if (np < 0) np += k.len;
                const uint64_t P = period16(k);
                const uint64_t want = phaseOfPos(k, uint32_t(np), second) | (ph & 0xFFFFu);
                k.offset16 = (k.offset16 % P + (want + P - ph)) % P;
                restartGrains(k, uint64_t(np) << 16);
            }
            break;
        case kMode: setMode(t, PlayMode(m.arg >= 0 && m.arg < kModeCount ? m.arg : 0)); break;
        case kMix: setMix(MixParam((uint16_t(m.arg) >> 8) & 0xFF), t, int(m.arg & 0xFF)); break;
        case kOpt: setOpt(OptParam((uint16_t(m.arg) >> 8) & 0xFF), t, int(m.arg & 0xFF)); break;
        case kPerf: {
            const int fx = (uint16_t(m.arg) >> 8) & 0xFF;
            if (fx < kPerfFxCount) setPerf(t, PerfFx(fx), (m.arg & 1) != 0);
            break;
        }
        case kPerfOff:
            for (Track& x : trk_) {
                x.perf.mask = 0;
                x.perf.repFx = 0xFF;
            }
            break;
        case kPerfParam: {
            const int fx = (uint16_t(m.arg) >> 8) & 0xFF;
            if (fx >= kPerfFxCount) break;
            const int v = m.arg & 0xFF;
            perfParam_[fx] = uint8_t(v > kPerfKnobs[fx].max ? kPerfKnobs[fx].max : v);
            // A repeat that is playing takes its new length at once.
            for (Track& x : trk_)
                if (x.perf.repFx == fx && ((x.perf.mask >> fx) & 1)) startRepeat(x, PerfFx(fx));
            break;
        }
        case kFxType:
            if (m.track < kFxSlots && m.arg >= 0 && m.arg < kFxTypeCount) fx_[m.track].setType(FxType(m.arg));
            break;
        case kFxKnob:
            if (m.track < kFxSlots) fx_[m.track].setKnob((uint16_t(m.arg) >> 8) & 0xFF, uint8_t(m.arg & 0xFF));
            break;
        case kVolume: k.volume = uint8_t(m.arg < 0 ? 0 : (m.arg > 100 ? 100 : m.arg)); break;
        case kTransport:
            if (running_) {
                for (int o = 0; o < kTracks; ++o)
                    if (trk_[o].state == TrackState::kRecording || trk_[o].state == TrackState::kCountIn ||
                        trk_[o].state == TrackState::kArmed)
                        finishNow(o);
                running_ = false;
            } else if (L_) {
                // Everything from bar 1: what was playing plays, what was queued
                // starts now, stopped tracks stay stopped.
                setClock(0);
                running_ = true;
                for (Track& x : trk_)
                    if (x.state == TrackState::kQueued) {
                        x.anchor = 0;
                        launch(x);
                    }
            }
            break;
        case kClearAll:
            for (int o = 0; o < kTracks; ++o) clearTrack(o);
            break;
        case kMonitor: monitor_ = m.arg != 0; break;
        case kFeedback: feedback_ = uint8_t(m.arg < 0 ? 0 : (m.arg > 100 ? 100 : m.arg)); break;
    }
}

void Engine::setMix(MixParam p, int t, int v) {
    auto pct = [](int x, int hi) { return uint8_t(x < 0 ? 0 : (x > hi ? hi : x)); };
    switch (p) {
        case MixParam::kLevel: trk_[t].volume = pct(v, 100); break;
        case MixParam::kSend1: send_[t][0] = pct(v, 100); break;
        case MixParam::kSend2: send_[t][1] = pct(v, 100); break;
        case MixParam::kReturn1: return_[0] = pct(v, 100); break;
        case MixParam::kReturn2: return_[1] = pct(v, 100); break;
        case MixParam::kMaster: masterLevel_ = pct(v, 100); break;
    }
}

void Engine::setOpt(OptParam p, int t, int v) {
    auto lim = [](int x, int lo, int hi) { return uint8_t(x < lo ? lo : (x > hi ? hi : x)); };
    TrackOpts& o = opts_[t];
    const bool empty = trk_[t].state == TrackState::kEmpty || trk_[t].state == TrackState::kClearing;
    switch (p) {
        case OptParam::kStop: o.stop = StopQuant(lim(v, 0, 2)); break;
        case OptParam::kLength: o.lenBeats = lim(v, 0, kMaxLenBeats); break;
        case OptParam::kStart: o.start = StartMode(lim(v, 0, 2)); break;
        case OptParam::kInput:   // the buffer layout changes: only while the track is empty
            if (empty) o.input = InputCh(lim(v, 0, 3));
            break;
        case OptParam::kAfter: o.after = AfterRec(lim(v, 0, 2)); break;
        case OptParam::kTempo:
            // Before there is a loop the tempo sets the bar; after, it sets the
            // tape speed (50 .. 200 % of the recorded tempo; the pitch follows).
            if (L_ == 0) {
                bpm_ = lim(v, 40, 240);
            } else {
                const float base = baseTempo();
                if (v == int(base + 0.5f)) {
                    speed16_ = 65536;
                } else {
                    const float s = float(v) / base * 65536.0f;
                    speed16_ = uint32_t(s < 32768.0f ? 32768.0f : (s > 131072.0f ? 131072.0f : s));
                }
                for (Track& k : trk_)   // only Forward overdubs off speed 1
                    if (k.state == TrackState::kOverdubbing && !canDubNow(k)) k.state = TrackState::kPlaying;
            }
            break;
        case OptParam::kBeats: beats_ = lim(v, 1, 12); break;
        case OptParam::kMetronome: metro_ = v != 0; break;
        case OptParam::kClick: click_ = lim(v, 0, 100); break;
        case OptParam::kThreshold: threshold_ = lim(v, 0, 100); break;
    }
}

void Engine::startRepeat(Track& k, PerfFx r) {
    // Repeat slice: its knob's note length, on the track's beat grid, around
    // the position playing now.
    Perf& p = k.perf;
    const uint8_t v = perfParam_[int(r)];
    uint32_t slice = beatLen() / kRepeatDiv[v < sizeof kRepeatDiv ? v : 0];
    if (slice < 64) slice = 64;
    if (slice > k.len) slice = k.len;
    const uint32_t bar = barLen();
    const int64_t origin = bar ? int64_t((bar - k.anchor % bar) % bar) : 0;   // a bar line in the buffer
    const int64_t pos = posOf(k);
    const int64_t d = pos - origin;
    int64_t s = (d >= 0 ? d / slice : -((-d + slice - 1) / slice)) * int64_t(slice) + origin;
    s %= int64_t(k.len);
    if (s < 0) s += k.len;
    p.repStart = uint32_t(s);
    p.repSlice = slice;
    p.repT0 = t_;
    p.repIn = uint32_t((pos - s + int64_t(k.len)) % int64_t(k.len)) % slice;
    p.repFx = uint8_t(r);
}

void Engine::startClockAlone(int t) {
    // The bar clock starts for track t only: loops that were playing or
    // waiting go to stopped, to be started on a bar like any stopped track.
    for (int o = 0; o < kTracks; ++o) {
        Track& x = trk_[o];
        if (o != t && (x.state == TrackState::kPlaying || x.state == TrackState::kOverdubbing ||
                       x.state == TrackState::kQueued))
            x.state = TrackState::kStopped;
    }
    setClock(0);
    running_ = true;
}

void Engine::launch(Track& k) {
    k.offset16 = 0;
    k.state = k.launchDub ? TrackState::kOverdubbing : TrackState::kPlaying;
    restartGrains(k, uint64_t(posOf(k)) << 16);
}

void Engine::queueLaunch(int t, bool dub) {
    Track& k = trk_[t];
    k.launchDub = dub && canDubNow(k);
    if (!running_) {   // nothing running: this track starts the clock, from its top
        startClockAlone(t);
        k.anchor = 0;
        launch(k);
        return;
    }
    // From its beginning on the next bar line (now, when exactly on one).
    const uint32_t bar = barLen();
    k.anchor = bar ? ((t_ + bar - 1) / bar) * bar : t_;
    k.offset16 = 0;
    k.state = TrackState::kQueued;
}

void Engine::setPerf(int t, PerfFx fx, bool on) {
    Track& k = trk_[t];
    Perf& p = k.perf;
    const uint8_t bit = uint8_t(1u << int(fx));
    const uint8_t repBits = uint8_t((1u << int(PerfFx::kRepeat8)) | (1u << int(PerfFx::kRepeat16)));
    if (!on) {
        p.mask &= uint8_t(~bit);
        if ((bit & repBits) && p.repFx == uint8_t(fx)) {
            // The other repeat may still be held: it takes over from here.
            if (p.mask & repBits)
                startRepeat(k, (p.mask & (1u << int(PerfFx::kRepeat8))) ? PerfFx::kRepeat8 : PerfFx::kRepeat16);
            else p.repFx = 0xFF;
        }
        return;
    }
    if (!k.len || (p.mask & bit)) return;   // only a track with a loop
    p.mask |= bit;
    switch (fx) {
        case PerfFx::kRepeat8:
        case PerfFx::kRepeat16: startRepeat(k, fx); break;
        case PerfFx::kTapeStop:
            p.tapeBase = posOf(k);
            p.tapePos = 0.0f;
            p.tapeRate = 1.0f;
            break;
        case PerfFx::kVibrato: p.vib.start(5.5f); break;
        case PerfFx::kSweepDown: p.lpT0 = t_; break;
        case PerfFx::kSweepUp: p.hpT0 = t_; break;
        default: break;
    }
}

uint8_t Engine::opt(OptParam p, int t) const {
    const TrackOpts& o = opts_[t];
    switch (p) {
        case OptParam::kStop: return uint8_t(o.stop);
        case OptParam::kLength: return o.lenBeats;
        case OptParam::kStart: return uint8_t(o.start);
        case OptParam::kInput: return uint8_t(o.input);
        case OptParam::kAfter: return uint8_t(o.after);
        case OptParam::kTempo: {
            // With a loop: the loop's tempo at the tape speed.
            if (!L_) return bpm_;
            const float b = baseTempo() * speed() + 0.5f;
            return uint8_t(b > 255.0f ? 255.0f : b);
        }
        case OptParam::kBeats: return beats_;
        case OptParam::kMetronome: return metro_;
        case OptParam::kClick: return click_;
        case OptParam::kThreshold: return threshold_;
    }
    return 0;
}

uint8_t Engine::mix(MixParam p, int t) const {
    switch (p) {
        case MixParam::kLevel: return trk_[t].volume;
        case MixParam::kSend1: return send_[t][0];
        case MixParam::kSend2: return send_[t][1];
        case MixParam::kReturn1: return return_[0];
        case MixParam::kReturn2: return return_[1];
        case MixParam::kMaster: return masterLevel_;
    }
    return 0;
}

void Engine::render(int16_t* out, const int16_t* in, uint32_t frames) {
    // Commands first, so a press acts on this block.
    uint32_t tail = qTail_.load(std::memory_order_relaxed);
    while (tail != qHead_.load(std::memory_order_acquire)) {
        apply(queue_[tail]);
        tail = (tail + 1) % kQueue;
        qTail_.store(tail, std::memory_order_release);
    }
    if (job_.state.load(std::memory_order_acquire) == 2) finishJob();
    if (running_)
        for (Track& k : trk_)   // queued tracks start on their bar line
            if (k.state == TrackState::kQueued && k.anchor <= t_) launch(k);
    for (int t = 0; t < kTracks; ++t)
        if (trk_[t].state == TrackState::kClearing && zeroTodo_[t].load(std::memory_order_acquire) == 0) {
            trk_[t].state = TrackState::kEmpty;
            trk_[t].written = 0;
        }

    std::memset(out, 0, size_t(frames) * 2 * sizeof(int16_t));
    float mixL[128], mixR[128];
    const uint32_t n = frames > 128 ? 128 : frames;
    float (&busL)[kFxSlots][128] = busL_;   // send buses (post-fader), members: off the ISR stack
    float (&busR)[kFxSlots][128] = busR_;
    std::memset(busL_, 0, sizeof busL_);
    std::memset(busR_, 0, sizeof busR_);
    uint8_t inPk[2] = {0, 0};
    for (uint32_t f = 0; f < n; ++f) {
        const int16_t l = in ? in[2 * f] : 0, r = in ? in[2 * f + 1] : 0;
        const uint8_t pl = peak8(l, 0), pr = peak8(r, 0);
        if (pl > inPk[0]) inPk[0] = pl;
        if (pr > inPk[1]) inPk[1] = pr;
        mixL[f] = monitor_ ? float(l) : 0.0f;
        mixR[f] = monitor_ ? float(r) : 0.0f;
    }

    const float fb = float(feedback_) / 100.0f;
    // Sound start: the input level that triggers an armed track.
    const float thrAmp = 32768.0f * std::pow(10.0f, thresholdDb(threshold_) / 20.0f);
    for (int t = 0; t < kTracks; ++t) {
        Track& k = trk_[t];
        const TrackMemory& m = mem_[t];
        uint8_t level = 0;
        bool wrote = false;
        uint32_t fStart = 0;
        if (k.state == TrackState::kArmed && in) {
            // The first frame over the threshold starts the take, with the
            // pre-roll (the last kPreRollFrames before it) at its head.
            uint32_t f = 0;
            for (; f < n; ++f) {
                const float a = std::fabs(float(in[2 * f])), b = std::fabs(float(in[2 * f + 1]));
                if (a >= thrAmp || b >= thrAmp) break;
            }
            if (f < n) {
                int16_t pre[kPreRollFrames * 2];
                const uint32_t fromRing = kPreRollFrames - f;   // f < 128 < kPreRollFrames
                std::memcpy(pre, preRoll_ + 2 * f, fromRing * 2 * sizeof(int16_t));
                std::memcpy(pre + 2 * fromRing, in, f * 2 * sizeof(int16_t));
                if (L_ == 0 || !running_) {
                    // No loop running: the bar clock starts at the pre-roll; the
                    // other loops stay stopped.
                    startClockAlone(t);
                    setClock(kPreRollFrames);   // frame f sits at the end of the pre-roll
                    tAcc_ -= uint64_t(f) * speed16_;
                    t_ = uint32_t(tAcc_ >> 16);
                }
                beginTake(t, tf(f), pre, kPreRollFrames);
                fStart = f;
            }
        }
        if ((k.state == TrackState::kRecording || k.state == TrackState::kCountIn) && running_) {
            const uint32_t cap = capOf(t);
            const uint32_t sp = speed16_;
            bool done = false;
            auto put = [&](uint32_t i, int16_t l, int16_t r) {
                if (k.mono) {
                    const int16_t v = fold(k.input, l, r);
                    m.frames[i] = v;
                    notePeakMono(m.peaks, i, v);
                } else {
                    m.frames[2 * i] = l;
                    m.frames[2 * i + 1] = r;
                    notePeak(m.peaks, i, l, r);
                }
            };
            for (uint32_t f = fStart; f < n; ++f) {
                const int16_t l = in ? in[2 * f] : 0, r = in ? in[2 * f + 1] : 0;
                if (k.state == TrackState::kCountIn) {   // starts on its bar line, to the frame
                    if (tf(f) < k.anchor) continue;
                    k.state = TrackState::kRecording;
                    k.written = 0;
                    k.recPos16 = int64_t(tAcc_ + uint64_t(f) * sp) - (int64_t(k.anchor) << 16);
                    k.prevL = l;
                    k.prevR = r;
                }
                const uint32_t i = k.written;
                if (i >= cap || (k.stopAt && i >= k.stopAt)) {
                    done = true;
                    break;
                }
                if (sp == 65536) {
                    put(i, l, r);
                    k.written = i + 1;
                    k.recPos16 = int64_t(k.written) << 16;
                } else {
                    // Varispeed: the input lands on the loop's timeline, resampled
                    // to the buffer frames between the last input frame and this one.
                    const int64_t hi = k.recPos16, lo = hi - int64_t(sp);
                    for (int64_t x = (lo >> 16) + 1; x <= (hi >> 16); ++x) {
                        if (x < int64_t(k.written)) continue;
                        const uint32_t j = uint32_t(x);
                        if (j >= cap || (k.stopAt && j >= k.stopAt)) {
                            done = true;
                            break;
                        }
                        const float a = float(x * 65536 - lo) / float(sp);
                        put(j, sat16(lerp(k.prevL, l, a)), sat16(lerp(k.prevR, r, a)));
                        k.written = j + 1;
                    }
                    k.recPos16 = hi + int64_t(sp);
                    if (done) break;
                }
                k.prevL = l;
                k.prevR = r;
            }
            if (k.state == TrackState::kRecording) {
                if (k.stopAt && k.written >= k.stopAt) finishRecording(t, k.stopAt);
                else if (done || k.written >= cap) finishNow(t);
                wrote = true;
            }
        } else if (running_ && k.len && !k.busy &&   // busy: a cut is moving the audio, the track is silent
                   (k.state == TrackState::kPlaying || k.state == TrackState::kOverdubbing ||
                    k.state == TrackState::kStopped)) {
            const ModeInfo& mi = modeInfo(k.mode);
            Perf& pf = k.perf;
            auto has = [&](PerfFx x) { return (pf.mask >> int(x)) & 1u; };
            const bool rep = (has(PerfFx::kRepeat8) || has(PerfFx::kRepeat16)) && pf.repFx != 0xFF && pf.repSlice;
            const bool tape = has(PerfFx::kTapeStop), vib = has(PerfFx::kVibrato);
            const bool posFx = rep || tape || vib;
            // Overdub writes where it reads: not while a performance effect moves the read position.
            const bool dub = k.state == TrackState::kOverdubbing && mi.canDub && !posFx;
            const float target = (k.state == TrackState::kStopped || k.muted) ? 0.0f : float(k.volume) / 100.0f;

            // Filters: a cut holds its knob's cutoff; a sweep moves over its knob's
            // time (in bars) while held.
            const float bar = float(barLen());
            const bool lpOn = has(PerfFx::kLowPass) || has(PerfFx::kSweepDown);
            const bool hpOn = has(PerfFx::kHighPass) || has(PerfFx::kSweepUp);
            auto sweepLen = [&](PerfFx x) {
                const uint8_t v = perfParam_[int(x)];
                return bar * kSweepBars[v < sizeof kSweepBars / sizeof kSweepBars[0] ? v : 2];
            };
            float lpFc = 20000.0f, hpFc = 20.0f;
            if (has(PerfFx::kSweepDown)) {
                const float len = sweepLen(PerfFx::kSweepDown);
                const float pr = len > 0 ? std::fmin(1.0f, float(t_ - pf.lpT0) / len) : 1.0f;
                lpFc = 18000.0f * std::pow(200.0f / 18000.0f, pr);
            }
            if (has(PerfFx::kLowPass)) lpFc = std::fmin(lpFc, lowPassHz(perfParam_[int(PerfFx::kLowPass)]));
            if (has(PerfFx::kSweepUp)) {
                const float len = sweepLen(PerfFx::kSweepUp);
                const float pr = len > 0 ? std::fmin(1.0f, float(t_ - pf.hpT0) / len) : 1.0f;
                hpFc = 20.0f * std::pow(4000.0f / 20.0f, pr);
            }
            if (has(PerfFx::kHighPass)) hpFc = std::fmax(hpFc, highPassHz(perfParam_[int(PerfFx::kHighPass)]));
            // Wet amounts ramp over a block (no clicks on press / release).
            const float lpW0 = pf.lpWet, hpW0 = pf.hpWet;
            pf.lpWet = lpOn ? 1.0f : 0.0f;
            pf.hpWet = hpOn ? 1.0f : 0.0f;
            const bool lpRun = lpOn || lpW0 > 0.0f, hpRun = hpOn || hpW0 > 0.0f;
            const float lpWs = (pf.lpWet - lpW0) / float(n), hpWs = (pf.hpWet - hpW0) / float(n);
            float lpW = lpW0, hpW = hpW0;
            struct Coef {
                float a1, a2, a3, k;
            };
            auto coef = [](float fc) {
                const float gg = std::tan(3.14159265f * std::fmin(fc, 20000.0f) / float(kSampleRate));
                const float kk = 0.8f;   // a little resonance
                Coef c;
                c.k = kk;
                c.a1 = 1.0f / (1.0f + gg * (gg + kk));
                c.a2 = gg * c.a1;
                c.a3 = gg * c.a2;
                return c;
            };
            const Coef lpc = coef(lpFc), hpc = coef(hpFc);
            // One channel of an SVF; returns low-pass or high-pass.
            auto svf = [](Svf& s, int ch, const Coef& c, float x, bool high) {
                const float v3 = x - s.ic2[ch];
                const float v1 = c.a1 * s.ic1[ch] + c.a2 * v3;
                const float v2 = s.ic2[ch] + c.a2 * s.ic1[ch] + c.a3 * v3;
                s.ic1[ch] = 2.0f * v1 - s.ic1[ch];
                s.ic2[ch] = 2.0f * v2 - s.ic2[ch];
                return high ? x - c.k * v1 - v2 : v2;
            };
            // Tape stop over its knob's time: 1 beat, 2 beats, 1 bar, 2 bars.
            static const uint8_t kTapeBeats[] = {1, 2, 0, 0};
            const uint8_t tv = perfParam_[int(PerfFx::kTapeStop)] & 3u;
            const uint32_t tapeFrames = kTapeBeats[tv] ? beatLen() * kTapeBeats[tv] : barLen() * (tv == 2 ? 1u : 2u);
            // Vibrato depth: up to +-60 frames (about +-80 cents at 5.5 Hz).
            const float vibDepth = 0.6f * float(perfParam_[int(PerfFx::kVibrato)]);
            // At a changed tempo every reader moves speed16_ times as far per output frame.
            const uint32_t sp = speed16_;
            const bool vari = sp != 65536;
            const uint64_t step16 = (uint64_t(mi.time16) * sp) >> 16;
            const uint64_t pitch16 = (uint64_t(mi.pitch16) * sp) >> 16;
            const float tapeStep = float(sp) / 65536.0f / float(tapeFrames ? tapeFrames : 1u);
            const float speed = float(step16) / 65536.0f;
            const float step = (target - k.gain) / float(n);
            float g = k.gain;
            float sg[kFxSlots], sstep[kFxSlots];
            for (int s = 0; s < kFxSlots; ++s) {
                const float st = float(send_[t][s]) / 100.0f;
                sg[s] = sendGain_[t][s];
                sstep[s] = (st - sg[s]) / float(n);
                sendGain_[t][s] = st;
            }
            const uint64_t P = period16(k), L16 = uint64_t(k.len) << 16;
            uint64_t ph = phase16(k);
            // Frame i as left/right; a mono buffer gives the same sample to both.
            const bool mono = k.mono;
            // Loop frame i is buffer frame start + i (a trim moves start).
            int16_t* const fr = m.frames + (mono ? size_t(k.start) : size_t(k.start) * 2);
            const uint32_t pk0 = k.start;   // peak cells use buffer frames
            auto lr = [&](uint32_t i, int16_t& a, int16_t& b) {
                if (mono) a = b = fr[i];
                else {
                    a = fr[2 * i];
                    b = fr[2 * i + 1];
                }
            };
            auto interp = [&](uint32_t i, uint32_t j, float fr, float& l, float& r) {
                int16_t al, ar, bl, br;
                lr(i, al, ar);
                lr(j, bl, br);
                l = lerp(al, bl, fr);
                r = lerp(ar, br, fr);
            };
            for (uint32_t f = 0; f < n; ++f) {
                float sl = 0.0f, sr = 0.0f;
                if (posFx) {
                    // Performance read position: repeat slice, tape stop or the normal
                    // one, then vibrato on top; always interpolated.
                    uint32_t ip;
                    float fp = 0.0f, env = 1.0f;
                    if (rep) {
                        const uint32_t e = (tf(f) - pf.repT0 + pf.repIn) % pf.repSlice;
                        ip = (pf.repStart + e) % k.len;
                        // 64-frame fades at the slice edges keep the repeats click-free.
                        const float a = float(e) / 64.0f, z = float(pf.repSlice - e) / 64.0f;
                        env = std::fmin(1.0f, std::fmin(a, z));
                    } else if (tape) {
                        pf.tapeRate = std::fmax(0.0f, pf.tapeRate - tapeStep);
                        pf.tapePos += pf.tapeRate * speed;
                        const uint32_t whole = uint32_t(pf.tapePos);
                        ip = (pf.tapeBase + whole) % k.len;
                        fp = pf.tapePos - float(whole);
                        env = pf.tapeRate;   // fades out as it slows
                    } else {
                        ip = posOfPhase(k, ph);
                        if (mi.reader != ModeInfo::kDirect) fp = float(uint32_t(ph & 0xFFFFu)) * (1.0f / 65536.0f);
                    }
                    if (vib) {
                        // 5.5 Hz, depth from its knob.
                        const float o = vibDepth * pf.vib.next();
                        float pos = float(ip) + fp + o;
                        if (pos < 0.0f) pos += float(k.len);
                        if (pos >= float(k.len)) pos -= float(k.len);
                        ip = uint32_t(pos);
                        if (ip >= k.len) ip = k.len - 1;
                        fp = pos - float(ip);
                    }
                    const uint32_t j = ip + 1 < k.len ? ip + 1 : 0;
                    interp(ip, j, fp, sl, sr);
                    sl *= env;
                    sr *= env;
                } else
                switch (mi.reader) {
                    case ModeInfo::kDirect: {   // speed 1: forward, reverse, ping-pong; overdub writes here
                        if (vari) {
                            // Varispeed: between this frame and the next one along the phase.
                            const uint64_t ph2 = ph + 65536 < P ? ph + 65536 : ph + 65536 - P;
                            const float fp = float(uint32_t(ph & 0xFFFFu)) * (1.0f / 65536.0f);
                            interp(posOfPhase(k, ph), posOfPhase(k, ph2), fp, sl, sr);
                            if (dub) dubVari(t, ph, step16, in ? in + 2 * f : nullptr, fb);
                            break;
                        }
                        const uint32_t i = posOfPhase(k, ph);
                        int16_t a, b;
                        lr(i, a, b);
                        sl = a;
                        sr = b;
                        if (dub) {
                            const int16_t l = in ? in[2 * f] : 0, r = in ? in[2 * f + 1] : 0;
                            if (mono) {
                                const int16_t nv = sat16(float(a) * fb + float(fold(k.input, l, r)));
                                fr[i] = nv;
                                notePeakMono(m.peaks, pk0 + i, nv);
                            } else {
                                const int16_t nl = sat16(float(a) * fb + float(l));
                                const int16_t nr = sat16(float(b) * fb + float(r));
                                fr[2 * i] = nl;
                                fr[2 * i + 1] = nr;
                                notePeak(m.peaks, pk0 + i, nl, nr);
                            }
                        }
                        break;
                    }
                    case ModeInfo::kTape: {     // varispeed: pitch follows speed
                        const uint32_t i = uint32_t(ph >> 16), j = i + 1 < k.len ? i + 1 : 0;
                        const float fr = float(uint32_t(ph & 0xFFFFu)) * (1.0f / 65536.0f);
                        interp(i, j, fr, sl, sr);
                        break;
                    }
                    case ModeInfo::kGrains: {   // tempo from the phase, pitch from the grain read rate
                        for (Grain& gr : k.grain) {
                            if (gr.age >= kGrainFrames) gr = Grain{ph, 0};
                            const float w = gWin[gr.age * kWinSize / kGrainFrames];
                            const uint32_t i = uint32_t(gr.pos16 >> 16), j = i + 1 < k.len ? i + 1 : 0;
                            const float fr = float(uint32_t(gr.pos16 & 0xFFFFu)) * (1.0f / 65536.0f);
                            float gl, gr2;
                            interp(i, j, fr, gl, gr2);
                            sl += w * gl;
                            sr += w * gr2;
                            gr.pos16 += pitch16;
                            if (gr.pos16 >= L16) gr.pos16 -= L16;
                            ++gr.age;
                        }
                        break;
                    }
                }
                if (lpRun) {
                    lpW += lpWs;
                    sl += (svf(pf.lp, 0, lpc, sl, false) - sl) * lpW;
                    sr += (svf(pf.lp, 1, lpc, sr, false) - sr) * lpW;
                }
                if (hpRun) {
                    hpW += hpWs;
                    sl += (svf(pf.hp, 0, hpc, sl, true) - sl) * hpW;
                    sr += (svf(pf.hp, 1, hpc, sr, true) - sr) * hpW;
                }
                ph += step16;
                if (ph >= P) ph -= P;
                g += step;
                const float ol = sl * g, orr = sr * g;
                mixL[f] += ol;
                mixR[f] += orr;
                for (int s = 0; s < kFxSlots; ++s) {
                    sg[s] += sstep[s];
                    busL[s][f] += ol * sg[s];
                    busR[s][f] += orr * sg[s];
                }
                const uint8_t p = peak8(int(ol), int(orr));
                if (p > level) level = p;
            }
            k.gain = target;
            wrote = dub;
            if (vib) pf.vib.renorm();
        }
        if (wrote) view_[t].peakVersion.fetch_add(1, std::memory_order_relaxed);
        view_[t].level.store(level, std::memory_order_relaxed);
    }

    // Metronome: always (when on) or during any track's count-in. Accent on the
    // bar line. Dry, after the sends, so the effects never hear it.
    bool counting = false;
    for (const Track& k : trk_) counting = counting || k.state == TrackState::kCountIn;
    if (running_ && (metro_ || counting) && click_) {
        const uint32_t bar = barLen(), beat = beatLen();
        const float lvl = float(click_) / 100.0f * 9000.0f;
        for (uint32_t f = 0; f < n; ++f) {
            // A click where the clock crosses into a new beat (at a changed tempo
            // it moves in parts of frames, so it may step over the exact frame).
            const uint32_t bi = beat ? tf(f) / beat : 0;
            if (beat && bi != lastBeat_) {
                lastBeat_ = bi;
                clickAge_ = 0;
                const uint32_t beats = bar / beat ? bar / beat : 1;
                clickOsc_.start(bi % beats == 0 ? 1600.0f : 1000.0f);
                clickAmp_ = 1.0f;
            }
            if (clickAge_ < kSampleRate / 20) {   // 50 ms, ~10 ms decay
                const float v = clickOsc_.next() * clickAmp_ * lvl;
                clickAmp_ *= 0.9977f;
                ++clickAge_;
                mixL[f] += v;
                mixR[f] += v;
            }
        }
        clickOsc_.renorm();
    } else {
        clickAge_ = 0xFFFFFFFFu;
        lastBeat_ = running_ && beatLen() ? tf(0) / beatLen() : 0xFFFFFFFFu;   // no click for a beat already under way
    }

    // Send effects run every block (tails ring out while the transport is stopped).
    {
        // The delay times follow the tempo: a bar in output frames.
        const uint32_t bar = uint32_t(uint64_t(barLen()) * 65536u / speed16_);
        float wetL[128], wetR[128];
        for (int s = 0; s < kFxSlots; ++s) {
            fx_[s].process(busL[s], busR[s], wetL, wetR, n, bar);
            const float rt = float(return_[s]) / 100.0f;
            float rg = returnGain_[s];
            const float rstep = (rt - rg) / float(n);
            returnGain_[s] = rt;
            uint8_t pk = 0;
            for (uint32_t f = 0; f < n; ++f) {
                rg += rstep;
                const float l = wetL[f] * rg, r = wetR[f] * rg;
                mixL[f] += l;
                mixR[f] += r;
                const uint8_t p = peak8(int(sat16(l)), int(sat16(r)));
                if (p > pk) pk = p;
            }
            retPeak_[s].store(pk, std::memory_order_relaxed);
        }
    }

    uint8_t outPk[2] = {0, 0};
    const float mt = float(masterLevel_) / 100.0f;
    float mg = masterGain_;
    const float mstep = (mt - mg) / float(n);
    masterGain_ = mt;
    for (uint32_t f = 0; f < n; ++f) {
        mg += mstep;
        out[2 * f] = sat16(mixL[f] * mg);
        out[2 * f + 1] = sat16(mixR[f] * mg);
        const uint8_t pl = peak8(out[2 * f], 0), pr = peak8(out[2 * f + 1], 0);
        if (pl > outPk[0]) outPk[0] = pl;
        if (pr > outPk[1]) outPk[1] = pr;
    }
    if (running_) {   // speed16_ loop frames per output frame
        tAcc_ += uint64_t(n) * speed16_;
        t_ = uint32_t(tAcc_ >> 16);
    }
    // Keep the last kPreRollFrames of input for a sound-started take.
    std::memmove(preRoll_, preRoll_ + 2 * n, (kPreRollFrames - n) * 2 * sizeof(int16_t));
    if (in) std::memcpy(preRoll_ + 2 * (kPreRollFrames - n), in, n * 2 * sizeof(int16_t));
    else std::memset(preRoll_ + 2 * (kPreRollFrames - n), 0, n * 2 * sizeof(int16_t));
    for (int c = 0; c < 2; ++c) {
        inPeak_[c].store(inPk[c], std::memory_order_relaxed);
        outPeak_[c].store(outPk[c], std::memory_order_relaxed);
    }
    publish();
}

void Engine::publish() {
    for (int t = 0; t < kTracks; ++t) {
        const Track& k = trk_[t];
        TrackView& v = view_[t];
        v.state.store(uint8_t(k.state), std::memory_order_relaxed);
        v.muted.store(k.muted, std::memory_order_relaxed);
        v.mode.store(uint8_t(k.mode), std::memory_order_relaxed);
        v.len.store(k.len, std::memory_order_relaxed);
        v.written.store(k.state == TrackState::kRecording ? k.written : k.len, std::memory_order_relaxed);
        // Stopped and queued tracks show their start: that is where they begin again.
        const bool parked = k.state == TrackState::kStopped || k.state == TrackState::kQueued;
        v.pos.store(k.state == TrackState::kRecording ? k.written : (parked ? 0 : posOf(k)), std::memory_order_relaxed);
        v.mono.store(k.mono, std::memory_order_relaxed);
        v.perf.store(k.perf.mask, std::memory_order_relaxed);
        v.start.store(k.start, std::memory_order_relaxed);
        v.busy.store(k.busy, std::memory_order_relaxed);
        v.canUndo.store(k.undo.has, std::memory_order_relaxed);
        v.startIn.store(k.state == TrackState::kCountIn && k.anchor > t_ ? k.anchor - t_ : 0,
                        std::memory_order_relaxed);
        v.stopIn.store(k.state == TrackState::kRecording && k.stopAt > k.written ? k.stopAt - k.written : 0,
                       std::memory_order_relaxed);
        // Buffer index 0 is at global frame `anchor`: the first bar line is where
        // the global grid crosses the buffer.
        const uint32_t bar = barLen();
        v.barOffset.store(bar ? (bar - k.anchor % bar) % bar : 0, std::memory_order_relaxed);
    }
    bar_.store(barLen(), std::memory_order_relaxed);
    master_.store(L_, std::memory_order_relaxed);
    tPub_.store(t_, std::memory_order_relaxed);
    runPub_.store(running_, std::memory_order_relaxed);
}

}  // namespace looper
