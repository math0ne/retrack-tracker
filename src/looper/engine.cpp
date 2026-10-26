#include "looper/engine.h"

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

inline uint8_t peak8(int a, int b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    const int m = a > b ? a : b;
    return uint8_t(m >> 7);
}

}  // namespace

void Engine::init(const TrackMemory mem[kTracks]) {
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
        std::memset(mem_[t].peaks + c0, 0, c1 - c0);
        zeroTodo_[t].store(from, std::memory_order_release);
        return;   // one chunk per call keeps the main loop responsive
    }
}

bool Engine::allEmpty() const {
    for (const Track& k : trk_)
        if (k.state != TrackState::kEmpty && k.state != TrackState::kClearing) return false;
    return true;
}

uint32_t Engine::indexOf(const Track& k) const {
    if (!k.len) return 0;
    int64_t d = (int64_t(t_) - int64_t(k.anchor) + int64_t(k.offset)) % int64_t(k.len);
    if (d < 0) d += k.len;
    return uint32_t(d);
}

void Engine::startRecording(int t) {
    Track& k = trk_[t];
    if (L_ == 0) {
        // A second track pressed while the master is still recording closes
        // the master first, so there is only ever one master take.
        for (int o = 0; o < kTracks; ++o)
            if (o != t && trk_[o].state == TrackState::kRecording) finishRecording(o);
    }
    if (L_ == 0) {
        t_ = 0;
        running_ = true;
        k.anchor = 0;
    } else {
        if (!running_) {
            t_ = 0;
            running_ = true;
        }
        k.anchor = (t_ / L_) * L_;   // bar start; the frames before t_ stay silent (buffer is zeroed)
    }
    k.recStart = t_;
    k.len = 0;
    k.offset = 0;
    k.written = t_ - k.anchor;
    k.state = TrackState::kRecording;
}

void Engine::finishRecording(int t) {
    Track& k = trk_[t];
    const uint32_t n = k.written;
    if (L_ == 0) {
        if (n < kMinMasterFrames) {
            clearTrack(t);
            return;
        }
        L_ = n;
        k.len = n;
    } else {
        // Whole bars; a take that ends less than an eighth of a bar past a bar
        // line was meant to stop on it (late press), so it rounds down.
        uint32_t bars = n / L_;
        if (bars == 0 || n % L_ > L_ / kSegments) ++bars;
        uint32_t len = bars * L_;
        if (len > mem_[t].capacity) len = (mem_[t].capacity / L_) * L_;
        k.len = len;
    }
    // Frames past len (a late press) were written too: the next clear zeroes them.
    k.written = n > k.len ? n : k.len;
    k.state = TrackState::kPlaying;
}

void Engine::clearTrack(int t) {
    Track& k = trk_[t];
    const uint8_t vol = k.volume;
    const uint32_t dirty = k.written;
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
                case TrackState::kRecording: finishRecording(t); break;
                case TrackState::kPlaying:
                case TrackState::kStopped:
                    k.state = TrackState::kOverdubbing;
                    if (!running_) running_ = true;
                    break;
                case TrackState::kOverdubbing: k.state = TrackState::kPlaying; break;
                case TrackState::kClearing: break;
            }
            break;
        case kPlayStop:
            switch (k.state) {
                case TrackState::kRecording: finishRecording(t); break;
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
            if (k.len >= kSegments && m.arg >= 0 && m.arg < kSegments) {
                const uint32_t seg = k.len / kSegments;
                const uint32_t cur = indexOf(k) / seg;
                int64_t off = int64_t(k.offset) + (int64_t(m.arg) - int64_t(cur)) * seg;
                off %= int64_t(k.len);
                if (off < 0) off += k.len;
                k.offset = uint32_t(off);
            }
            break;
        case kVolume: k.volume = uint8_t(m.arg < 0 ? 0 : (m.arg > 100 ? 100 : m.arg)); break;
        case kTransport:
            if (running_) {
                for (int o = 0; o < kTracks; ++o)
                    if (trk_[o].state == TrackState::kRecording) finishRecording(o);
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
        if (k.state == TrackState::kRecording && running_) {
            uint32_t i = k.written;
            bool full = false;
            for (uint32_t f = 0; f < n; ++f, ++i) {
                if (i >= m.capacity) {
                    full = true;
                    break;
                }
                const int16_t l = in ? in[2 * f] : 0, r = in ? in[2 * f + 1] : 0;
                m.frames[2 * i] = l;
                m.frames[2 * i + 1] = r;
                const uint32_t cell = i / kPeakFrames;
                if (i % kPeakFrames == 0) m.peaks[cell] = 0;
                const uint8_t p = peak8(l, r);
                if (p > m.peaks[cell]) m.peaks[cell] = p;
            }
            k.written = i;
            wrote = true;
            if (full) finishRecording(t);
        } else if (running_ && k.len &&
                   (k.state == TrackState::kPlaying || k.state == TrackState::kOverdubbing ||
                    k.state == TrackState::kStopped)) {
            const bool dub = k.state == TrackState::kOverdubbing;
            const float target = (k.state == TrackState::kStopped || k.muted) ? 0.0f : float(k.volume) / 100.0f;
            const float step = (target - k.gain) / float(n);
            float g = k.gain;
            uint32_t i = indexOf(k);
            for (uint32_t f = 0; f < n; ++f) {
                const int16_t sl = m.frames[2 * i], sr = m.frames[2 * i + 1];
                if (dub) {
                    const int16_t l = in ? in[2 * f] : 0, r = in ? in[2 * f + 1] : 0;
                    const int16_t nl = sat16(float(sl) * fb + float(l));
                    const int16_t nr = sat16(float(sr) * fb + float(r));
                    m.frames[2 * i] = nl;
                    m.frames[2 * i + 1] = nr;
                    const uint32_t cell = i / kPeakFrames;
                    if (i % kPeakFrames == 0) m.peaks[cell] = 0;
                    const uint8_t p = peak8(nl, nr);
                    if (p > m.peaks[cell]) m.peaks[cell] = p;
                }
                g += step;
                const float ol = float(sl) * g, orr = float(sr) * g;
                mixL[f] += ol;
                mixR[f] += orr;
                const uint8_t p = peak8(int(ol), int(orr));
                if (p > level) level = p;
                if (++i == k.len) i = 0;
            }
            k.gain = target;
            wrote = dub;
        }
        if (wrote) view_[t].peakVersion.fetch_add(1, std::memory_order_relaxed);
        view_[t].level.store(level, std::memory_order_relaxed);
    }

    uint8_t outPk[2] = {0, 0};
    for (uint32_t f = 0; f < n; ++f) {
        out[2 * f] = sat16(mixL[f]);
        out[2 * f + 1] = sat16(mixR[f]);
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
        v.len.store(k.len, std::memory_order_relaxed);
        v.written.store(k.state == TrackState::kRecording ? k.written : k.len, std::memory_order_relaxed);
        v.pos.store(k.state == TrackState::kRecording ? k.written : indexOf(k), std::memory_order_relaxed);
    }
    master_.store(L_, std::memory_order_relaxed);
    tPub_.store(t_, std::memory_order_relaxed);
    runPub_.store(running_, std::memory_order_relaxed);
}

}  // namespace looper
