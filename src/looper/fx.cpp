#include "looper/fx.h"

#include <cmath>
#include <cstring>

namespace looper {

namespace {

constexpr float kFs = 44100.0f;

inline int16_t sat16(float v) {
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return int16_t(v);
}

// Line lengths in frames: spread, mutually prime-ish, 23..65 ms.
constexpr uint32_t kLineLen[Reverb::kLines] = {1031, 1327, 1523, 1801, 2053, 2311, 2579, 2861};
constexpr uint32_t kApLen[Reverb::kDiffusers] = {142, 379, 107, 277};   // L, L, R, R
constexpr float kApGain = 0.65f;
constexpr float kInGain = 0.25f;
// Output gain: measured with steady noise, a send at the default size (RT60
// 2.1 s) comes back about 3 dB below its input, like the delay and chorus. A
// longer tail stacks up more energy, so the gain falls a little with the decay
// (small rooms are not much quieter than big ones).
constexpr float kOutGain = 1.37f;
constexpr float kOutRt = 2.1f;   // the decay the gain is set for

static_assert(1031 + 1327 + 1523 + 1801 + 2053 + 2311 + 2579 + 2861 + 142 + 379 + 107 + 277 <= Reverb::kFloats,
              "reverb memory");
static_assert(ModDelay::kFrames * 4 <= Delay::kBytes, "the modulated delay uses the slot's delay line");

constexpr uint32_t kClearChunk = 8192;   // bytes zeroed per block on a type change

}  // namespace

const char* const kDelayDivNames[kDelayDivCount] = {"1/32", "1/16", "1/8", "3/16", "1/4", "3/8", "1/2", "3/4"};

// Knobs per type: name, max, default, value list.
const char* const kOffOn[2] = {"Off", "On"};

const FxTypeInfo kFxTypes[kFxTypeCount] = {
    {"Delay", {{"Time", kDelayDivCount - 1, 2, kDelayDivNames}, {"Feedback", 95, 40, nullptr},
               {"Tone", 100, 55, nullptr}, {"Spread", 100, 0, nullptr}}},
    {"Ping-pong", {{"Time", kDelayDivCount - 1, 2, kDelayDivNames}, {"Feedback", 95, 50, nullptr},
                   {"Tone", 100, 55, nullptr}, {"Width", 100, 100, nullptr}}},
    {"Reverb", {{"Size", 100, 60, nullptr}, {"Tone", 100, 50, nullptr}, {"Width", 100, 100, nullptr},
                {nullptr, 0, 0, nullptr}}},
    {"Chorus", {{"Rate", 100, 30, nullptr}, {"Depth", 100, 50, nullptr}, {"Width", 100, 80, nullptr},
                {nullptr, 0, 0, nullptr}}},
    {"Flanger", {{"Rate", 100, 20, nullptr}, {"Depth", 100, 60, nullptr}, {"Feedback", 95, 60, nullptr},
                 {"Manual", 100, 30, nullptr}}},
    {"Clouds", {{"Position", 100, 20, nullptr}, {"Size", 100, 40, nullptr}, {"Pitch", 48, kCloudsPitchZero, nullptr},
                {"Density", 100, 80, nullptr}, {"Texture", 100, 40, nullptr}, {"Feedback", 100, 0, nullptr},
                {"Spread", 100, 50, nullptr}, {"Freeze", 1, 0, kOffOn}}},
};

// ---------------------------------------------------------------------------
// Osc

void Osc::start(float hz) {
    s = 0.0f;
    c = 1.0f;
    setRate(hz);
}

void Osc::setRate(float hz) {
    const float w = 6.2831853f * hz / kFs;
    ds = std::sin(w);
    dc = std::cos(w);
}

void Osc::renorm() {
    const float m = s * s + c * c;
    if (m > 0.0f) {
        const float r = 1.0f / std::sqrt(m);
        s *= r;
        c *= r;
    }
}

// ---------------------------------------------------------------------------
// Delay

void Delay::reset() { lpL_ = lpR_ = 0.0f; }

void Delay::setTime(uint32_t frames, float spread) {
    auto clampT = [](float f) { return f < 64.0f ? 64.0f : (f > float(kMaxFrames - 4) ? float(kMaxFrames - 4) : f); };
    targetL_ = clampT(float(frames));
    targetR_ = clampT(float(frames) * (1.0f + 0.25f * spread));
}

void Delay::process(const float* inL, const float* inR, float* outL, float* outR, uint32_t n) {
    auto tap = [&](float time, int ch) {
        float rp = float(write_) - time;
        if (rp < 0.0f) rp += float(kMaxFrames);
        const uint32_t i = uint32_t(rp);
        const uint32_t j = i + 1 < kMaxFrames ? i + 1 : 0;
        const float fr = rp - float(i);
        return float(line_[2 * i + ch]) + (float(line_[2 * j + ch]) - float(line_[2 * i + ch])) * fr;
    };
    for (uint32_t f = 0; f < n; ++f) {
        timeL_ += (targetL_ - timeL_) * 0.0002f;   // ~110 ms glide
        timeR_ += (targetR_ - timeR_) * 0.0002f;
        const float dl = tap(timeL_, 0), dr = tap(timeR_, 1);
        // Darker repeats: one-pole low-pass in the feedback path.
        lpL_ += (dl - lpL_) * lpK_;
        lpR_ += (dr - lpR_) * lpK_;
        float wl, wr;
        if (pingPong_) {
            // The input enters on the left; each echo crosses to the other side.
            const float mono = (inL[f] + inR[f]) * 0.5f;
            wl = mono + lpR_ * fb_;
            wr = lpL_ * fb_;
        } else {
            wl = inL[f] + lpL_ * fb_;
            wr = inR[f] + lpR_ * fb_;
        }
        line_[2 * write_] = sat16(wl);
        line_[2 * write_ + 1] = sat16(wr);
        if (++write_ == kMaxFrames) write_ = 0;
        // Width: 0 = both sides the same, 1 = as is.
        const float mid = (dl + dr) * 0.5f;
        outL[f] = mid + (dl - mid) * width_;
        outR[f] = mid + (dr - mid) * width_;
    }
}

// ---------------------------------------------------------------------------
// Reverb

void Reverb::init(float* mem) {
    mem_ = mem;
    float* p = mem_;
    for (int i = 0; i < kLines; ++i) {
        line_[i] = Line{p, kLineLen[i], 0};
        p += kLineLen[i];
    }
    for (int i = 0; i < kDiffusers; ++i) {
        ap_[i] = Line{p, kApLen[i], 0};
        p += kApLen[i];
    }
    reset();
    set(60, 50, 100);
}

void Reverb::reset() {
    for (float& v : lp_) v = 0.0f;
}

void Reverb::set(uint8_t size, uint8_t tone, uint8_t width) {
    width_ = float(width) / 100.0f;
    if (size == size_ && tone == tone_) return;
    size_ = size;
    tone_ = tone;
    // RT60 0.3 s .. 9 s, exponential in `size`; per-line gain for -60 dB after RT60.
    const float rt60 = 0.3f * std::pow(30.0f, float(size) / 100.0f);
    for (int i = 0; i < kLines; ++i) gain_[i] = std::pow(10.0f, -3.0f * float(kLineLen[i]) / (rt60 * kFs));
    damp_ = 0.12f + 0.88f * float(tone) / 100.0f;
    out_ = kOutGain * std::pow(rt60 / kOutRt, -0.2f);
}

void Reverb::process(const float* inL, const float* inR, float* outL, float* outR, uint32_t n) {
    for (uint32_t f = 0; f < n; ++f) {
        // Input diffusion: two Schroeder allpasses per side.
        float in[2] = {inL[f] * kInGain, inR[f] * kInGain};
        for (int s = 0; s < 2; ++s)
            for (int k = 0; k < 2; ++k) {
                Line& a = ap_[s * 2 + k];
                const float d = a.buf[a.idx];
                const float y = d - kApGain * in[s];
                a.buf[a.idx] = in[s] + kApGain * y;
                if (++a.idx == a.len) a.idx = 0;
                in[s] = y;
            }
        // Read, damp, mix (Householder: x - 2/N * sum), write back with decay.
        float x[kLines];
        float sum = 0.0f;
        for (int i = 0; i < kLines; ++i) {
            const float v = line_[i].buf[line_[i].idx];
            lp_[i] += (v - lp_[i]) * damp_;
            x[i] = lp_[i];
            sum += x[i];
        }
        const float h = sum * (2.0f / float(kLines));
        float l = 0.0f, r = 0.0f;
        for (int i = 0; i < kLines; ++i) {
            Line& ln = line_[i];
            ln.buf[ln.idx] = (x[i] - h) * gain_[i] + in[i & 1];
            if (++ln.idx == ln.len) ln.idx = 0;
            if (i & 1) r += x[i];
            else l += x[i];
        }
        l *= out_;
        r *= out_;
        const float mid = (l + r) * 0.5f;
        outL[f] = mid + (l - mid) * width_;
        outR[f] = mid + (r - mid) * width_;
    }
}

// ---------------------------------------------------------------------------
// ModDelay (chorus / flanger)

void ModDelay::reset() {
    lastL_ = lastR_ = 0.0f;
    lfoL_.start(rate_ > 0.0f ? rate_ : 0.5f);
}

void ModDelay::set(bool flanger, float rateHz, float depth, float width, float fb, float manual) {
    flanger_ = flanger;
    if (rateHz != rate_) {
        rate_ = rateHz;
        lfoL_.setRate(rateHz);
    }
    if (flanger) {
        base_ = (0.5f + 7.5f * manual) * kFs / 1000.0f;   // 0.5 .. 8 ms
        depth_ = base_ * 0.9f * depth;
        fb_ = fb;
    } else {
        base_ = 15.0f * kFs / 1000.0f;                    // 15 ms +- up to 8 ms
        depth_ = 8.0f * kFs / 1000.0f * depth;
        fb_ = 0.0f;
    }
    width_ = width;
}

void ModDelay::process(const float* inL, const float* inR, float* outL, float* outR, uint32_t n) {
    auto tap = [&](float d, int ch) {
        float rp = float(write_) - d;
        if (rp < 0.0f) rp += float(kFrames);
        const uint32_t i = uint32_t(rp) % kFrames;
        const uint32_t j = i + 1 < kFrames ? i + 1 : 0;
        const float fr = rp - float(uint32_t(rp));
        return float(line_[2 * i + ch]) + (float(line_[2 * j + ch]) - float(line_[2 * i + ch])) * fr;
    };
    for (uint32_t f = 0; f < n; ++f) {
        // Right LFO: the left one turned by up to a quarter cycle (width).
        const float sl = lfoL_.next(), cl = lfoL_.cosNow();
        const float sr = sl * (1.0f - width_) + cl * width_;
        const float dl = tap(base_ + depth_ * (0.5f + 0.5f * sl), 0);
        const float dr = tap(base_ + depth_ * (0.5f + 0.5f * sr), 1);
        line_[2 * write_] = sat16(inL[f] + dl * fb_);
        line_[2 * write_ + 1] = sat16(inR[f] + dr * fb_);
        if (++write_ == kFrames) write_ = 0;
        outL[f] = dl;
        outR[f] = dr;
    }
    lfoL_.renorm();
}

// ---------------------------------------------------------------------------
// FxUnit

void FxUnit::init(int16_t* line, float* reverbMem) {
    line_ = line;
    delay_.init(line);
    mod_.init(line);
    reverb_.init(reverbMem);
    clouds_.init(line);   // Delay::kBytes = CloudsFx::kFrames stereo int16
    // Zeroed once at start-up, before the audio runs.
    std::memset(line, 0, Delay::kBytes);
    std::memset(reverbMem, 0, Reverb::kBytes);
    setType(type_);
    clearTodo_ = revTodo_ = 0;
}

void FxUnit::setType(FxType t) {
    type_ = t;
    const FxTypeInfo& ti = fxTypeInfo(t);
    for (int i = 0; i < kFxKnobs; ++i) knob_[i] = ti.knob[i].name ? ti.knob[i].def : 0;
    // The old effect's sound is still in the lines: quiet until they are zero.
    clearTodo_ = Delay::kBytes;
    revTodo_ = Reverb::kBytes;
    delay_.reset();
    reverb_.reset();
    mod_.reset();
    clouds_.reset();
}

void FxUnit::setKnob(int i, uint8_t v) {
    if (i < 0 || i >= kFxKnobs) return;
    const FxKnob& k = fxTypeInfo(type_).knob[i];
    if (!k.name) return;
    knob_[i] = v > k.max ? k.max : v;
}

void FxUnit::apply(uint32_t bar) {
    const float k1 = float(knob_[1]) / 100.0f, k2 = float(knob_[2]) / 100.0f, k3 = float(knob_[3]) / 100.0f;
    switch (type_) {
        case FxType::kDelay:
        case FxType::kPingPong: {
            const DelayDiv& dv = kDelayDivs[knob_[0] < kDelayDivCount ? knob_[0] : 0];
            uint32_t dt = uint32_t(uint64_t(bar ? bar : 88200u) * dv.num / dv.den);
            while (dt > Delay::kMaxFrames - 4) dt /= 2;
            const bool pp = type_ == FxType::kPingPong;
            delay_.setTime(dt, pp ? 0.0f : k3);
            delay_.set(k1, k2, pp, pp ? k3 : 1.0f);
            break;
        }
        case FxType::kReverb: reverb_.set(knob_[0], knob_[1], knob_[2]); break;
        case FxType::kClouds: {
            auto u = [&](int i) { return float(knob_[i]) / 100.0f; };
            clouds_.set(u(0), u(1), float(int(knob_[2]) - kCloudsPitchZero), u(3), u(4), u(5), u(6),
                        knob_[kCloudsFreeze] != 0);
            break;
        }
        case FxType::kChorus:
        case FxType::kFlanger: {
            const bool fl = type_ == FxType::kFlanger;
            const float r0 = float(knob_[0]) / 100.0f;
            // Rate: chorus 0.1..5 Hz, flanger 0.05..2 Hz, exponential.
            const float rate = fl ? 0.05f * std::pow(40.0f, r0) : 0.1f * std::pow(50.0f, r0);
            mod_.set(fl, rate, k1, fl ? 1.0f : k2, fl ? k2 : 0.0f, k3);
            break;
        }
        default: break;
    }
}

void FxUnit::process(const float* inL, const float* inR, float* outL, float* outR, uint32_t n, uint32_t bar) {
    if (clearTodo_ || revTodo_) {
        // Zero the lines a chunk per block, silent meanwhile (~0.2 s for a delay line).
        if (clearTodo_) {
            const uint32_t c = clearTodo_ < kClearChunk ? clearTodo_ : kClearChunk;
            clearTodo_ -= c;
            std::memset(reinterpret_cast<uint8_t*>(line_) + clearTodo_, 0, c);
        }
        if (revTodo_) {
            const uint32_t c = revTodo_ < kClearChunk ? revTodo_ : kClearChunk;
            revTodo_ -= c;
            std::memset(reinterpret_cast<uint8_t*>(reverb_.memory()) + revTodo_, 0, c);
        }
        for (uint32_t f = 0; f < n; ++f) outL[f] = outR[f] = 0.0f;
        return;
    }
    apply(bar);
    switch (type_) {
        case FxType::kDelay:
        case FxType::kPingPong: delay_.process(inL, inR, outL, outR, n); break;
        case FxType::kReverb: reverb_.process(inL, inR, outL, outR, n); break;
        case FxType::kClouds: clouds_.process(inL, inR, outL, outR, n); break;
        default: mod_.process(inL, inR, outL, outR, n); break;
    }
}

}  // namespace looper
