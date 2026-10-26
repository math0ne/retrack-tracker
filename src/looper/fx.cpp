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
constexpr float kInGain = 0.25f, kOutGain = 0.45f;

static_assert(1031 + 1327 + 1523 + 1801 + 2053 + 2311 + 2579 + 2861 + 142 + 379 + 107 + 277 <= Reverb::kFloats,
              "reverb memory");

}  // namespace

// ---------------------------------------------------------------------------
// Delay

void Delay::init(int16_t* line) {
    line_ = line;
    clear();
}

void Delay::clear() {
    if (line_) std::memset(line_, 0, kBytes);
    lpL_ = lpR_ = 0.0f;
}

void Delay::setTime(uint32_t frames) {
    if (frames < 64) frames = 64;
    if (frames > kMaxFrames - 4) frames = kMaxFrames - 4;
    target_ = float(frames);
}

void Delay::process(const float* inL, const float* inR, float* outL, float* outR, uint32_t n) {
    for (uint32_t f = 0; f < n; ++f) {
        time_ += (target_ - time_) * 0.0002f;   // ~110 ms glide
        float rp = float(write_) - time_;
        if (rp < 0.0f) rp += float(kMaxFrames);
        const uint32_t i = uint32_t(rp);
        const uint32_t j = i + 1 < kMaxFrames ? i + 1 : 0;
        const float fr = rp - float(i);
        const float dl = float(line_[2 * i]) + (float(line_[2 * j]) - float(line_[2 * i])) * fr;
        const float dr = float(line_[2 * i + 1]) + (float(line_[2 * j + 1]) - float(line_[2 * i + 1])) * fr;
        // Darker repeats: one-pole low-pass in the feedback path.
        lpL_ += (dl - lpL_) * 0.55f;
        lpR_ += (dr - lpR_) * 0.55f;
        const float l = inL[f], r = inR[f];
        line_[2 * write_] = sat16(l + lpL_ * fb_);
        line_[2 * write_ + 1] = sat16(r + lpR_ * fb_);
        if (++write_ == kMaxFrames) write_ = 0;
        outL[f] = dl;
        outR[f] = dr;
    }
}

// ---------------------------------------------------------------------------
// Reverb

void Reverb::init(float* mem) {
    mem_ = mem;
    float* p = mem;
    for (int i = 0; i < kLines; ++i) {
        line_[i] = Line{p, kLineLen[i], 0};
        p += kLineLen[i];
    }
    for (int i = 0; i < kDiffusers; ++i) {
        ap_[i] = Line{p, kApLen[i], 0};
        p += kApLen[i];
    }
    clear();
    set(60, 50);
}

void Reverb::clear() {
    if (mem_) std::memset(mem_, 0, kBytes);
    for (float& v : lp_) v = 0.0f;
}

void Reverb::set(uint8_t size, uint8_t tone) {
    if (size == size_ && tone == tone_) return;
    size_ = size;
    tone_ = tone;
    // RT60 0.3 s .. 9 s, exponential in `size`; per-line gain for -60 dB after RT60.
    const float rt60 = 0.3f * std::pow(30.0f, float(size) / 100.0f);
    for (int i = 0; i < kLines; ++i) gain_[i] = std::pow(10.0f, -3.0f * float(kLineLen[i]) / (rt60 * kFs));
    damp_ = 0.12f + 0.88f * float(tone) / 100.0f;
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
        outL[f] = l * kOutGain;
        outR[f] = r * kOutGain;
    }
}

}  // namespace looper
