// Send effects. Each of the two FX slots holds one FxUnit, which can be any of
// the FxType effects; its knobs are 0..100 values (or list indices) whose
// meaning depends on the type (kFxTypes). Everything runs inside
// Engine::render (audio thread) on 128-frame float blocks. Delay lines are
// handed in at init (SDRAM on the device); the reverb keeps its lines in
// internal SRAM, one array per slot.
#pragma once

#include <cstdint>

namespace looper {

// Sine by rotation: four multiply-adds per sample instead of a sinf() call
// in the audio interrupt. renorm() once a block stops the amplitude drifting.
struct Osc {
    float s = 0.0f, c = 1.0f, ds = 0.0f, dc = 1.0f;
    void start(float hz);
    void setRate(float hz);   // keeps the phase
    float next() {
        const float v = s, ns = s * dc + c * ds;
        c = c * dc - s * ds;
        s = ns;
        return v;
    }
    float cosNow() const { return c; }
    void renorm();
};

enum class FxType : uint8_t { kDelay, kPingPong, kReverb, kChorus, kFlanger, kCount };
constexpr int kFxTypeCount = int(FxType::kCount);
constexpr int kFxKnobs = 4;

struct FxKnob {
    const char* name;   // nullptr = the type has no such knob
    uint8_t max;        // values 0..max (a list index when list != nullptr)
    uint8_t def;
    const char* const* list;
};
struct FxTypeInfo {
    const char* name;
    FxKnob knob[kFxKnobs];
};

// Delay time as a fraction of the bar. A time longer than the delay line is
// halved until it fits, so it stays on the grid.
struct DelayDiv {
    const char* name;
    uint8_t num, den;
};
constexpr DelayDiv kDelayDivs[] = {{"1/32", 1, 32}, {"1/16", 1, 16}, {"1/8", 1, 8}, {"3/16", 3, 16},
                                   {"1/4", 1, 4},   {"3/8", 3, 8},   {"1/2", 1, 2}, {"3/4", 3, 4}};
constexpr int kDelayDivCount = int(sizeof kDelayDivs / sizeof kDelayDivs[0]);
extern const char* const kDelayDivNames[kDelayDivCount];

extern const FxTypeInfo kFxTypes[kFxTypeCount];
inline const FxTypeInfo& fxTypeInfo(FxType t) { return kFxTypes[int(t) < kFxTypeCount ? int(t) : 0]; }

// Stereo delay on an int16 line, low-passed feedback, a time that glides to a
// new target (no clicks; the glide bends the repeats like tape). Ping-pong
// mode sends the echoes across from side to side.
class Delay {
public:
    static constexpr uint32_t kMaxFrames = 131072;   // 2.97 s
    static constexpr uint32_t kBytes = kMaxFrames * 2 * sizeof(int16_t);

    void init(int16_t* line) { line_ = line; }
    void setTime(uint32_t frames, float spread);   // spread 0..1: right side up to 25 % later
    void set(float fb, float tone, bool pingPong, float width) {
        fb_ = fb;
        lpK_ = 0.08f + 0.9f * tone;
        pingPong_ = pingPong;
        width_ = width;
    }
    void process(const float* inL, const float* inR, float* outL, float* outR, uint32_t n);
    void reset();             // state only; the line is cleared by FxUnit

private:
    int16_t* line_ = nullptr;
    uint32_t write_ = 0;
    float timeL_ = 11025.0f, timeR_ = 11025.0f, targetL_ = 11025.0f, targetR_ = 11025.0f;
    float fb_ = 0.4f, lpK_ = 0.55f, width_ = 1.0f;
    bool pingPong_ = false;
    float lpL_ = 0.0f, lpR_ = 0.0f;
};

// Feedback delay network: two allpass diffusers per side, eight damped lines
// mixed by a Householder matrix. Decay 0.3..9 s from size, damping from tone,
// stereo width of the output.
class Reverb {
public:
    static constexpr int kLines = 8;
    static constexpr int kDiffusers = 4;                  // 2 per side
    static constexpr uint32_t kFloats = 17000;            // all lines and diffusers
    static constexpr uint32_t kBytes = kFloats * sizeof(float);

    void init(float* mem);
    void set(uint8_t size, uint8_t tone, uint8_t width);  // 0..100 each
    void process(const float* inL, const float* inR, float* outL, float* outR, uint32_t n);
    void reset();
    float* memory() const { return mem_; }

private:
    struct Line {
        float* buf;
        uint32_t len, idx;
    };
    Line line_[kLines] = {};
    Line ap_[kDiffusers] = {};
    float gain_[kLines] = {};
    float lp_[kLines] = {};
    float damp_ = 0.5f, width_ = 1.0f, out_ = 1.0f;
    uint8_t size_ = 255, tone_ = 255;    // last applied (255 = not yet)
    float* mem_ = nullptr;
};

// Short modulated delay: chorus (15 ms +- depth, two LFOs apart by width) or
// flanger (0.5..8 ms, with feedback). Uses the start of the slot's delay line.
class ModDelay {
public:
    static constexpr uint32_t kFrames = 4096;   // 93 ms ring
    void init(int16_t* line) { line_ = line; }
    void set(bool flanger, float rateHz, float depth, float width, float fb, float manual);
    void process(const float* inL, const float* inR, float* outL, float* outR, uint32_t n);
    void reset();
    float lfo() const { return lfoL_.s; }        // for the screen: -1..1

private:
    int16_t* line_ = nullptr;
    uint32_t write_ = 0;
    bool flanger_ = false;
    float base_ = 660.0f, depth_ = 200.0f, fb_ = 0.0f;
    float width_ = 1.0f, rate_ = -1.0f;
    Osc lfoL_;   // the right side is this one turned by `width`
    float lastL_ = 0.0f, lastR_ = 0.0f;
};

// One FX slot: the effect in it, its knobs, and a click-free type change (the
// slot goes quiet while its lines are zeroed a chunk per block).
class FxUnit {
public:
    void init(int16_t* line, float* reverbMem);
    void setType(FxType t);
    void setKnob(int i, uint8_t v);
    FxType type() const { return type_; }
    uint8_t knob(int i) const { return knob_[i]; }
    bool busy() const { return clearTodo_ != 0; }
    float lfo() const { return mod_.lfo(); }
    // barFrames: the delay times follow the bar.
    void process(const float* inL, const float* inR, float* outL, float* outR, uint32_t n, uint32_t barFrames);

private:
    void apply(uint32_t barFrames);
    FxType type_ = FxType::kDelay;
    uint8_t knob_[kFxKnobs] = {};
    int16_t* line_ = nullptr;
    Delay delay_;
    Reverb reverb_;
    ModDelay mod_;
    uint32_t clearTodo_ = 0;   // bytes of the line still to zero (from the end)
    uint32_t revTodo_ = 0;     // bytes of the reverb lines still to zero
};

}  // namespace looper
