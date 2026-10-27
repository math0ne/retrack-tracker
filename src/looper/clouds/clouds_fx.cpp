// Clouds send effect. The granular part is Emilie Gillet's (MIT); the
// parameter mapping follows OpenTracker's GranularCloud, which follows
// Clouds' granular_processor.cc.
#include "looper/clouds/clouds_fx.h"

#include <cmath>
#include <cstring>

namespace looper {
namespace {
// Clouds' grain-size table is in 32 kHz samples: the same lengths here.
constexpr float kSizeScale = float(CloudsFx::kRate) / 32000.0f;

// Transparent up to 75 % of full scale, then a soft knee to 100 % (Clouds'
// SoftClip bends even quiet signals a little).
inline float limit(float x) {
    const float a = x < 0.0f ? -x : x;
    if (a <= 0.75f) return x;
    const float y = 0.75f + 0.25f * stmlib::SoftClip((a - 0.75f) * 4.0f);
    return x < 0.0f ? -y : y;
}
}  // namespace

void CloudsFx::init(int16_t* buffer) {
    buf_ = buffer;
    diffuser_.Init(diffMem_);
    reset();
}

void CloudsFx::reset() {
    head_ = 0;
    fbHpL_ = fbHpR_ = fbXL_ = fbXR_ = 0.0f;
    std::memset(fbL_, 0, sizeof fbL_);
    std::memset(fbR_, 0, sizeof fbR_);
    player_.Init(2, kMaxGrains);
    player_.set_size_scale(kSizeScale);
    std::memset(diffMem_, 0, sizeof diffMem_);
}

void CloudsFx::set(float position, float size, float pitch, float density, float texture, float feedback,
                   float spread, bool freeze) {
    position_ = position;
    size_ = size;
    pitch_ = pitch;
    density_ = density;
    texture_ = texture;
    feedback_ = feedback;
    spread_ = spread;
    freeze_ = freeze;
}

void CloudsFx::process(const float* inL, const float* inR, float* outL, float* outR, uint32_t n) {
    if (!buf_ || n > uint32_t(kBlockMax)) {
        for (uint32_t f = 0; f < n; ++f) outL[f] = outR[f] = 0.0f;
        return;
    }
    // Record (the bus at +-32768 to +-1), with Clouds' high-passed,
    // soft-limited feedback from the last block's output; frozen, the buffer
    // holds still and the grains keep reading it.
    if (!freeze_) {
        const float cut = (20.0f + 100.0f * feedback_ * feedback_) / float(kRate);
        float hp = 1.0f - cut * 6.28318f;
        hp = hp < 0.9f ? 0.9f : (hp > 0.9999f ? 0.9999f : hp);
        for (uint32_t i = 0; i < n; ++i) {
            const float xl = fbL_[i], xr = fbR_[i];
            fbHpL_ = hp * (fbHpL_ + xl - fbXL_);
            fbHpR_ = hp * (fbHpR_ + xr - fbXR_);
            fbXL_ = xl;
            fbXR_ = xr;
            float l = inL[i] * (1.0f / 32768.0f), r = inR[i] * (1.0f / 32768.0f);
            if (feedback_ > 0.0f) {
                l += feedback_ * (stmlib::SoftLimit(feedback_ * 1.4f * fbHpL_ + l) - l);
                r += feedback_ * (stmlib::SoftLimit(feedback_ * 1.4f * fbHpR_ + r) - r);
            }
            const uint32_t at = (head_ & (kFrames - 1)) * 2;
            buf_[at] = int16_t(stmlib::Clip16(int32_t(limit(l) * 32767.0f)));
            buf_[at + 1] = int16_t(stmlib::Clip16(int32_t(limit(r) * 32767.0f)));
            head_ = (head_ + 1) & (kFrames - 1);
        }
    }

    // The grains (Clouds' density deadband and texture -> window / diffusion).
    clouds::Parameters p = {};
    p.position = position_;
    p.size = size_;
    p.pitch = pitch_;
    p.density = density_;
    p.texture = texture_;
    p.dry_wet = 1.0f;
    p.stereo_spread = spread_;
    p.feedback = feedback_;
    p.freeze = freeze_;
    if (density_ >= 0.53f) {
        p.granular.overlap = (density_ - 0.53f) * 2.12f;
        p.granular.use_deterministic_seed = false;
    } else if (density_ <= 0.47f) {
        p.granular.overlap = (0.47f - density_) * 2.12f;
        p.granular.use_deterministic_seed = true;
    } else {
        p.granular.overlap = 0.0f;
        p.granular.use_deterministic_seed = true;
    }
    // Clouds aims for 40 x overlap^3 grains; with kMaxGrains of them, scale
    // the overlap so a knob setting asks for as many grains as Clouds' would
    // (24), and the full knob for all of them.
    {
        float o3 = p.granular.overlap * p.granular.overlap * p.granular.overlap * (24.0f / float(kMaxGrains));
        if (o3 > 1.0f) o3 = 1.0f;
        p.granular.overlap = std::cbrt(o3);
    }
    p.granular.window_shape = texture_ < 0.75f ? texture_ * 1.333f : 1.0f;
    p.granular.stereo_spread = spread_;
    player_.Play(buf_, int32_t(kFrames), int32_t(head_), p, grainOut_, size_t(n));
    for (uint32_t i = 0; i < n; ++i) {
        outL_[i] = grainOut_[2 * i];
        outR_[i] = grainOut_[2 * i + 1];
    }
    diffuser_.set_amount(texture_ > 0.75f ? (texture_ - 0.75f) * 4.0f : 0.0f);
    diffuser_.Process(outL_, outR_, size_t(n));
    // -3 dB, then the limiter: overlapping grains add up to well over the
    // input level, and the send bus must not clip.
    for (uint32_t i = 0; i < n; ++i) {
        fbL_[i] = outL_[i];
        fbR_[i] = outR_[i];
        outL[i] = limit(outL_[i] * 0.7f) * 32768.0f;
        outR[i] = limit(outR_[i] * 0.7f) * 32768.0f;
    }
}

}  // namespace looper
