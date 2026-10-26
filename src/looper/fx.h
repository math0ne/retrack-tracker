// Send effects: a stereo delay synced to the loop and a stereo FDN reverb.
// Both run inside Engine::render (audio thread) on 128-frame float blocks. The
// delay line is handed in at init (SDRAM on the device); the reverb keeps its
// lines in internal SRAM.
#pragma once

#include <cstdint>

namespace looper {

// Tape-style stereo delay: int16 line, low-passed feedback, and a delay time
// that glides to a new target (no clicks when the time changes; the glide
// bends the pitch of the repeats briefly, like a tape delay).
class Delay {
public:
    static constexpr uint32_t kMaxFrames = 131072;   // 2.97 s
    static constexpr uint32_t kBytes = kMaxFrames * 2 * sizeof(int16_t);

    void init(int16_t* line);
    void setTime(uint32_t frames);    // target, clamped to the line
    void setFeedback(float fb) { fb_ = fb; }
    // out = the wet signal only; in may alias out.
    void process(const float* inL, const float* inR, float* outL, float* outR, uint32_t n);
    void clear();

private:
    int16_t* line_ = nullptr;
    uint32_t write_ = 0;
    float time_ = 11025.0f, target_ = 11025.0f;
    float fb_ = 0.4f;
    float lpL_ = 0.0f, lpR_ = 0.0f;
};

// Feedback delay network: two allpass diffusers per side, eight damped lines
// mixed by a Householder matrix. Decay time 0.3..9 s from `size`, damping from `tone`.
class Reverb {
public:
    static constexpr int kLines = 8;
    static constexpr int kDiffusers = 4;                  // 2 per side
    static constexpr uint32_t kFloats = 17000;            // all lines and diffusers
    static constexpr uint32_t kBytes = kFloats * sizeof(float);

    void init();                             // lines in a static SRAM array (fx.cpp)
    void set(uint8_t size, uint8_t tone);    // 0..100 each
    void process(const float* inL, const float* inR, float* outL, float* outR, uint32_t n);
    void clear();

private:
    struct Line {
        float* buf;
        uint32_t len, idx;
    };
    Line line_[kLines] = {};
    Line ap_[kDiffusers] = {};
    float gain_[kLines] = {};
    float lp_[kLines] = {};
    float damp_ = 0.5f;
    uint8_t size_ = 255, tone_ = 255;    // last applied (255 = not yet)
    float* mem_ = nullptr;
};

}  // namespace looper
