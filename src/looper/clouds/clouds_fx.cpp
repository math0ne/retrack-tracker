// Clouds send effect. The granular part is Emilie Gillet's (MIT); the
// parameter mapping, sample rate, input gain and level follow OpenTracker's
// GranularCloud, which follows Clouds' granular_processor.cc.
#include "looper/clouds/clouds_fx.h"

#include <cmath>
#include <cstring>

namespace looper {
namespace {
constexpr float kDown = float(CloudsFx::kRate) / 44100.0f;   // internal samples per output frame

// Transparent up to 75 % of full scale, then a soft knee to 100 %: stands in
// for the master limiter the fork had after its returns.
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
    downPhase_ = upPos_ = 0.0f;
    lastInL_ = lastInR_ = prevOutL_ = prevOutR_ = 0.0f;
    fbHpL_ = fbHpR_ = fbXL_ = fbXR_ = fbLpL_ = fbLpR_ = 0.0f;
    std::memset(fbL_, 0, sizeof fbL_);
    std::memset(fbR_, 0, sizeof fbR_);
    fbN_ = 0;
    player_.Init(2, kMaxGrains);
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
    // Down to 32 kHz (linear, as the fork), the bus at +-32768 to +-1, with
    // the fork's input gain.
    int m = 0;
    for (uint32_t f = 0; f < n; ++f) {
        const float l = inL[f] * (kInputGain / 32768.0f), r = inR[f] * (kInputGain / 32768.0f);
        downPhase_ += kDown;
        if (downPhase_ >= 1.0f) {
            downPhase_ -= 1.0f;
            const float t = 1.0f - downPhase_ / kDown;   // where the internal sample falls between the two
            inL_[m] = lastInL_ + (l - lastInL_) * t;
            inR_[m] = lastInR_ + (r - lastInR_) * t;
            ++m;
        }
        lastInL_ = l;
        lastInR_ = r;
    }

    // Record, with Clouds' high-passed, soft-limited feedback; frozen, the
    // buffer holds still and the grains keep reading it.
    if (!freeze_) {
        const float cut = (20.0f + 100.0f * feedback_ * feedback_) / float(kRate);
        float hp = 1.0f - cut * 6.28318f;
        hp = hp < 0.9f ? 0.9f : (hp > 0.9999f ? 0.9999f : hp);
        for (int i = 0; i < m; ++i) {
            const int k = fbN_ ? (i < fbN_ ? i : fbN_ - 1) : 0;
            const float xl = fbN_ ? fbL_[k] : 0.0f, xr = fbN_ ? fbR_[k] : 0.0f;
            fbHpL_ = hp * (fbHpL_ + xl - fbXL_);
            fbHpR_ = hp * (fbHpR_ + xr - fbXR_);
            fbXL_ = xl;
            fbXR_ = xr;
            float l = inL_[i], r = inR_[i];
            l += feedback_ * (stmlib::SoftLimit(feedback_ * 1.4f * fbHpL_ + l) - l);
            r += feedback_ * (stmlib::SoftLimit(feedback_ * 1.4f * fbHpR_ + r) - r);
            const uint32_t at = (head_ & (kFrames - 1)) * 2;
            buf_[at] = int16_t(stmlib::Clip16(int32_t(stmlib::SoftClip(l) * 32767.0f)));
            buf_[at + 1] = int16_t(stmlib::Clip16(int32_t(stmlib::SoftClip(r) * 32767.0f)));
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
    // Clouds (and the fork) aim for 40 x overlap^3 grains; with kMaxGrains of
    // them, scale the overlap so a knob setting asks for as many grains as
    // there, capped at kMaxGrains.
    {
        float o3 = p.granular.overlap * p.granular.overlap * p.granular.overlap * (40.0f / float(kMaxGrains));
        if (o3 > 1.0f) o3 = 1.0f;
        p.granular.overlap = std::cbrt(o3);
    }
    p.granular.window_shape = texture_ < 0.75f ? texture_ * 1.333f : 1.0f;
    p.granular.stereo_spread = spread_;
    player_.Play(buf_, int32_t(kFrames), int32_t(head_), p, grainOut_, size_t(m));
    for (int i = 0; i < m; ++i) {
        outL_[i + 1] = grainOut_[2 * i];
        outR_[i + 1] = grainOut_[2 * i + 1];
    }
    diffuser_.set_amount(texture_ > 0.75f ? (texture_ - 0.75f) * 4.0f : 0.0f);
    diffuser_.Process(outL_ + 1, outR_ + 1, size_t(m));
    // The feedback tap goes through the same limiter as the output: a loop
    // of grains piling onto their own output (high feedback, pitched up)
    // stays bounded instead of running away into crackle.
    // A gentle low-pass too (about 7 kHz): pitched up, each pass would
    // otherwise move the sound an octave higher until it is hiss at the top.
    for (int i = 0; i < m; ++i) {
        fbLpL_ += 0.75f * (limit(outL_[i + 1]) - fbLpL_);
        fbLpR_ += 0.75f * (limit(outR_[i + 1]) - fbLpR_);
        fbL_[i] = fbLpL_;
        fbR_[i] = fbLpR_;
    }
    fbN_ = m;

    // Up to 44.1 kHz: index 0 is the previous block's last sample. Full level,
    // as the fork, through the limiter.
    outL_[0] = prevOutL_;
    outR_[0] = prevOutR_;
    for (uint32_t f = 0; f < n; ++f) {
        int i = int(upPos_);
        if (i > m - 1) i = m - 1 < 0 ? 0 : m - 1;
        const float t = upPos_ - float(i);
        const int j = i + 1 <= m ? i + 1 : i;
        outL[f] = limit(outL_[i] + (outL_[j] - outL_[i]) * t) * 32768.0f;
        outR[f] = limit(outR_[i] + (outR_[j] - outR_[i]) * t) * 32768.0f;
        upPos_ += kDown;
    }
    upPos_ -= float(m);
    if (upPos_ < 0.0f) upPos_ = 0.0f;
    if (m) {
        prevOutL_ = outL_[m];
        prevOutR_ = outR_[m];
    }
}

}  // namespace looper
