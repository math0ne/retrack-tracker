// Clouds as a send effect: the Mutable Instruments Clouds granular engine
// (looper/clouds, ported from the OpenTracker fork) around a 4.1 s stereo
// record buffer at 32 kHz. The send bus is resampled down to 32 kHz,
// recorded (unless frozen) with soft-limited feedback, played back by up to
// kMaxGrains grains and resampled back up to 44.1 kHz. Fully wet: the dry
// signal is the track itself, through the mixer.
#pragma once

#include <cstdint>

#include "looper/clouds/diffuser.h"
#include "looper/clouds/granular_sample_player.h"

namespace looper {

class CloudsFx {
public:
    static constexpr uint32_t kRate = 32000;
    static constexpr uint32_t kFrames = 131072;   // stereo int16: 512 KB, the size of an FX slot's line
    static constexpr int kMaxGrains = 12;         // of Clouds' 40: each grain reads the uncached SDRAM
    static constexpr int kBlockMax = 128;

    void init(int16_t* buffer);   // kFrames * 2 int16, zeroed by the caller
    void reset();                 // state only

    // 0..1 each, pitch in semitones.
    void set(float position, float size, float pitch, float density, float texture, float feedback,
             float spread, bool freeze);
    void process(const float* inL, const float* inR, float* outL, float* outR, uint32_t n);

    // For the screen: active grains (position 0..1 back from the write head,
    // envelope phase 0..1).
    int grains(clouds::GrainVisual* out, int maxOut) const { return player_.GetGrainStates(out, maxOut, kFrames); }
    uint32_t writeHead() const { return head_; }
    bool frozen() const { return freeze_; }

private:
    int16_t* buf_ = nullptr;
    uint32_t head_ = 0;
    clouds::GranularSamplePlayer player_;
    clouds::Diffuser diffuser_;
    float diffMem_[2048];

    float position_ = 0.2f, size_ = 0.4f, pitch_ = 0.0f, density_ = 0.8f;
    float texture_ = 0.4f, feedback_ = 0.2f, spread_ = 0.5f;
    bool freeze_ = false;

    // 44.1 -> 32 kHz: a phase over the input; 32 -> 44.1 kHz: a read position
    // over this block's internal samples, the last of the previous block first.
    float downPhase_ = 0.0f, lastInL_ = 0.0f, lastInR_ = 0.0f;
    float upPos_ = 0.0f, prevOutL_ = 0.0f, prevOutR_ = 0.0f;
    // Feedback: the previous block's output, high-passed (one pole).
    float fbHpL_ = 0.0f, fbHpR_ = 0.0f, fbXL_ = 0.0f, fbXR_ = 0.0f;
    float fbL_[kBlockMax] = {}, fbR_[kBlockMax] = {};
    int fbN_ = 0;
    float inL_[kBlockMax], inR_[kBlockMax];
    float outL_[kBlockMax + 1], outR_[kBlockMax + 1];
    float grainOut_[kBlockMax * 2];
};

}  // namespace looper
