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

}  // namespace

void Engine::Osc::start(float hz) {
    const float w = 6.2831853f * hz / float(kSampleRate);
    s = 0.0f;
    c = 1.0f;
    ds = std::sin(w);
    dc = std::cos(w);
}

void Engine::Osc::renorm() {
    const float m = s * s + c * c;
    if (m > 0.0f) {
        const float r = 1.0f / std::sqrt(m);
        s *= r;
        c *= r;
    }
}

void Engine::init(const TrackMemory mem[kTracks], uint8_t* fxMem) {
    delay_.init(reinterpret_cast<int16_t*>(fxMem));
    reverb_.init();   // its lines live in internal SRAM (fx.cpp)
    for (int s = 0; s < kFxSlots; ++s) retPeak_[s].store(0);
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

void Engine::post(Cmd c, uint8_t track, int16_t arg) {
    const uint32_t head = qHead_.load(std::memory_order_relaxed);
    const uint32_t next = (head + 1) % kQueue;
    if (next == qTail_.load(std::memory_order_acquire)) return;   // full: drop (UI rate is far below this)
    queue_[head] = Msg{uint8_t(c), track, arg};
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
    const int64_t d = (int64_t(t_) - int64_t(k.anchor)) * int64_t(modeInfo(k.mode).time16) + int64_t(k.offset16 % P);
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
    if (k.state == TrackState::kOverdubbing && !modeInfo(mode).canDub) k.state = TrackState::kPlaying;
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
    if (first || !running_) {
        t_ = 0;
        running_ = true;
    }
    const uint32_t bar = barLen(), beat = beatLen();
    k.mono = o.input != InputCh::kStereo;
    k.input = o.input;
    k.len = 0;
    k.offset16 = 0;
    k.stopAt = o.lenBeats ? uint32_t(o.lenBeats) * beat : 0;
    if (o.countIn) {
        // One bar of click, then record from the bar line. With loops already
        // running, the next bar line at least half a bar away.
        uint32_t start = first ? bar : ((t_ + bar - 1) / bar) * bar;
        if (!first && start - t_ < bar / 2) start += bar;
        k.anchor = start;
        k.written = 0;
        k.state = TrackState::kCountIn;
    } else {
        // Recording starts now; the buffer index 0 sits on the bar (or beat)
        // line before the press so the take stays on the grid. Free takes
        // (with no fixed length) are not on the grid: they start where they start.
        uint32_t unit = o.stop == StopQuant::kBar && !o.lenBeats ? bar : beat;
        if (o.stop == StopQuant::kFree && !o.lenBeats) unit = 0;
        k.anchor = unit ? (t_ / unit) * unit : t_;
        k.written = t_ - k.anchor;   // the frames before the press stay silent (buffer is zeroed)
        k.state = TrackState::kRecording;
    }
    k.recStart = t_;
}

void Engine::requestStop(int t) {
    Track& k = trk_[t];
    if (k.state == TrackState::kCountIn) {   // pressed again during the count-in: cancel
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
    if (k.state == TrackState::kCountIn) {
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
            k.state = modeInfo(k.mode).canDub ? TrackState::kOverdubbing : TrackState::kPlaying;
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
        t_ = 0;
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
                case TrackState::kRecording: requestStop(t); break;
                case TrackState::kPlaying:
                case TrackState::kStopped:
                    if (!modeInfo(k.mode).canDub) break;   // the app tells the user why
                    k.state = TrackState::kOverdubbing;
                    if (!running_) running_ = true;
                    break;
                case TrackState::kOverdubbing: k.state = TrackState::kPlaying; break;
                case TrackState::kClearing: break;
            }
            break;
        case kPlayStop:
            switch (k.state) {
                case TrackState::kCountIn:
                case TrackState::kRecording: requestStop(t); break;
                case TrackState::kPlaying:
                case TrackState::kOverdubbing: k.state = TrackState::kStopped; break;
                case TrackState::kStopped:
                    k.state = TrackState::kPlaying;
                    if (!running_) running_ = true;
                    break;
                default: break;
            }
            break;
        case kMute: k.muted = !k.muted; break;
        case kClear: clearTrack(t); break;
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
        case kVolume: k.volume = uint8_t(m.arg < 0 ? 0 : (m.arg > 100 ? 100 : m.arg)); break;
        case kTransport:
            if (running_) {
                for (int o = 0; o < kTracks; ++o)
                    if (trk_[o].state == TrackState::kRecording || trk_[o].state == TrackState::kCountIn)
                        finishNow(o);
                running_ = false;
            } else if (L_) {
                t_ = 0;
                running_ = true;
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
        case MixParam::kDelayTime: delayDiv_ = pct(v, kDelayDivCount - 1); break;
        case MixParam::kDelayFeedback: delayFb_ = pct(v, 95); break;
        case MixParam::kReverbSize: reverbSize_ = pct(v, 100); break;
        case MixParam::kReverbTone: reverbTone_ = pct(v, 100); break;
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
        case OptParam::kCountIn: o.countIn = v != 0; break;
        case OptParam::kInput:   // the buffer layout changes: only while the track is empty
            if (empty) o.input = InputCh(lim(v, 0, 3));
            break;
        case OptParam::kAfter: o.after = AfterRec(lim(v, 0, 2)); break;
        case OptParam::kTempo:   // the tempo sets the bar only until there is a loop
            if (L_ == 0) bpm_ = lim(v, 40, 240);
            break;
        case OptParam::kBeats: beats_ = lim(v, 1, 12); break;
        case OptParam::kMetronome: metro_ = v != 0; break;
        case OptParam::kClick: click_ = lim(v, 0, 100); break;
    }
}

void Engine::setPerf(int t, PerfFx fx, bool on) {
    Track& k = trk_[t];
    Perf& p = k.perf;
    const uint8_t bit = uint8_t(1u << int(fx));
    const uint8_t repBits = uint8_t((1u << int(PerfFx::kRepeat8)) | (1u << int(PerfFx::kRepeat16)));
    // Repeat slice: on the track's beat grid, around the position playing now.
    auto startRepeat = [&](PerfFx r) {
        uint32_t slice = beatLen() / (r == PerfFx::kRepeat8 ? 2u : 4u);
        if (slice < 64) slice = 64;
        if (slice > k.len) slice = k.len;
        const uint32_t bar = barLen();
        const int64_t origin = bar ? int64_t((bar - k.anchor % bar) % bar) : 0;   // a bar line in the buffer
        const int64_t pos = posOf(k);
        int64_t d = pos - origin;
        int64_t s = (d >= 0 ? d / slice : -((-d + slice - 1) / slice)) * int64_t(slice) + origin;
        s %= int64_t(k.len);
        if (s < 0) s += k.len;
        p.repStart = uint32_t(s);
        p.repSlice = slice;
        p.repT0 = t_;
        p.repIn = uint32_t((pos - s + int64_t(k.len)) % int64_t(k.len)) % slice;
        p.repFx = uint8_t(r);
    };
    if (!on) {
        p.mask &= uint8_t(~bit);
        if ((bit & repBits) && p.repFx == uint8_t(fx)) {
            // The other repeat may still be held: it takes over from here.
            if (p.mask & repBits)
                startRepeat((p.mask & (1u << int(PerfFx::kRepeat8))) ? PerfFx::kRepeat8 : PerfFx::kRepeat16);
            else p.repFx = 0xFF;
        }
        return;
    }
    if (!k.len || (p.mask & bit)) return;   // only a track with a loop
    p.mask |= bit;
    switch (fx) {
        case PerfFx::kRepeat8:
        case PerfFx::kRepeat16: startRepeat(fx); break;
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
        case OptParam::kCountIn: return o.countIn;
        case OptParam::kInput: return uint8_t(o.input);
        case OptParam::kAfter: return uint8_t(o.after);
        case OptParam::kTempo: {
            // With a loop the tempo follows its bar.
            if (!L_) return bpm_;
            const uint32_t b = uint32_t(uint64_t(kSampleRate) * 60u * beats_ / L_);
            return uint8_t(b > 255 ? 255 : b);
        }
        case OptParam::kBeats: return beats_;
        case OptParam::kMetronome: return metro_;
        case OptParam::kClick: return click_;
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
        case MixParam::kDelayTime: return delayDiv_;
        case MixParam::kDelayFeedback: return delayFb_;
        case MixParam::kReverbSize: return reverbSize_;
        case MixParam::kReverbTone: return reverbTone_;
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
    for (int t = 0; t < kTracks; ++t) {
        Track& k = trk_[t];
        const TrackMemory& m = mem_[t];
        uint8_t level = 0;
        bool wrote = false;
        if ((k.state == TrackState::kRecording || k.state == TrackState::kCountIn) && running_) {
            const uint32_t cap = capOf(t);
            bool done = false;
            for (uint32_t f = 0; f < n; ++f) {
                if (k.state == TrackState::kCountIn) {   // starts on its bar line, to the frame
                    if (t_ + f < k.anchor) continue;
                    k.state = TrackState::kRecording;
                    k.written = 0;
                }
                const uint32_t i = k.written;
                if (i >= cap || (k.stopAt && i >= k.stopAt)) {
                    done = true;
                    break;
                }
                const int16_t l = in ? in[2 * f] : 0, r = in ? in[2 * f + 1] : 0;
                if (k.mono) {
                    const int16_t v = fold(k.input, l, r);
                    m.frames[i] = v;
                    notePeakMono(m.peaks, i, v);
                } else {
                    m.frames[2 * i] = l;
                    m.frames[2 * i + 1] = r;
                    notePeak(m.peaks, i, l, r);
                }
                k.written = i + 1;
            }
            if (k.state == TrackState::kRecording) {
                if (k.stopAt && k.written >= k.stopAt) finishRecording(t, k.stopAt);
                else if (done || k.written >= cap) finishNow(t);
                wrote = true;
            }
        } else if (running_ && k.len &&
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

            // Filters: a cut holds its cutoff; a sweep moves across one bar while held.
            const float bar = float(barLen());
            const bool lpOn = has(PerfFx::kLowPass) || has(PerfFx::kSweepDown);
            const bool hpOn = has(PerfFx::kHighPass) || has(PerfFx::kSweepUp);
            float lpFc = 20000.0f, hpFc = 20.0f;
            if (has(PerfFx::kSweepDown)) {
                const float pr = bar > 0 ? std::fmin(1.0f, float(t_ - pf.lpT0) / bar) : 1.0f;
                lpFc = 18000.0f * std::pow(200.0f / 18000.0f, pr);
            }
            if (has(PerfFx::kLowPass)) lpFc = std::fmin(lpFc, 450.0f);
            if (has(PerfFx::kSweepUp)) {
                const float pr = bar > 0 ? std::fmin(1.0f, float(t_ - pf.hpT0) / bar) : 1.0f;
                hpFc = 20.0f * std::pow(4000.0f / 20.0f, pr);
            }
            if (has(PerfFx::kHighPass)) hpFc = std::fmax(hpFc, 1800.0f);
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
            const uint32_t tapeFrames = beatLen() * 2u;   // stops over two beats
            const float tapeStep = 1.0f / float(tapeFrames ? tapeFrames : 1u);
            const float speed = float(mi.time16) / 65536.0f;
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
            auto lr = [&](uint32_t i, int16_t& a, int16_t& b) {
                if (mono) a = b = m.frames[i];
                else {
                    a = m.frames[2 * i];
                    b = m.frames[2 * i + 1];
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
                        const uint32_t e = (t_ + f - pf.repT0 + pf.repIn) % pf.repSlice;
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
                        // 5.5 Hz, +-30 frames: about +-40 cents.
                        const float o = 30.0f * pf.vib.next();
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
                        const uint32_t i = posOfPhase(k, ph);
                        int16_t a, b;
                        lr(i, a, b);
                        sl = a;
                        sr = b;
                        if (dub) {
                            const int16_t l = in ? in[2 * f] : 0, r = in ? in[2 * f + 1] : 0;
                            if (mono) {
                                const int16_t nv = sat16(float(a) * fb + float(fold(k.input, l, r)));
                                m.frames[i] = nv;
                                notePeakMono(m.peaks, i, nv);
                            } else {
                                const int16_t nl = sat16(float(a) * fb + float(l));
                                const int16_t nr = sat16(float(b) * fb + float(r));
                                m.frames[2 * i] = nl;
                                m.frames[2 * i + 1] = nr;
                                notePeak(m.peaks, i, nl, nr);
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
                            gr.pos16 += mi.pitch16;
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
                ph += mi.time16;
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
            const uint32_t tf = t_ + f;
            if (beat && tf % beat == 0) {
                clickAge_ = 0;
                clickOsc_.start(tf % bar == 0 ? 1600.0f : 1000.0f);
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
    }

    // Send effects run every block (tails ring out while the transport is stopped).
    {
        const uint32_t base = barLen();
        const DelayDiv& dv = kDelayDivs[delayDiv_ < kDelayDivCount ? delayDiv_ : 0];
        uint32_t dt = uint32_t(uint64_t(base) * dv.num / dv.den);
        while (dt > Delay::kMaxFrames - 4) dt /= 2;
        delay_.setTime(dt);
        delay_.setFeedback(float(delayFb_) / 100.0f);
        reverb_.set(reverbSize_, reverbTone_);
        float wetL[128], wetR[128];
        for (int s = 0; s < kFxSlots; ++s) {
            if (s == 0) delay_.process(busL[0], busR[0], wetL, wetR, n);
            else reverb_.process(busL[1], busR[1], wetL, wetR, n);
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
    if (running_) t_ += n;
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
        v.pos.store(k.state == TrackState::kRecording ? k.written : posOf(k), std::memory_order_relaxed);
        v.mono.store(k.mono, std::memory_order_relaxed);
        v.perf.store(k.perf.mask, std::memory_order_relaxed);
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
