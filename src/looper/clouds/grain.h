// Single grain synthesis - ported from Mutable Instruments Clouds.
// Copyright 2014 Emilie Gillet (MIT License)
#pragma once

#include <cstdint>
#include "looper/clouds/stmlib_compat.h"
#include "looper/clouds/resources.h"

namespace clouds {

enum GrainQuality {
    GRAIN_QUALITY_LOW,
    GRAIN_QUALITY_MEDIUM,
    GRAIN_QUALITY_HIGH
};

class Grain {
public:
    Grain() { }
    ~Grain() { }

    void Init() {
        active_ = false;
        envelope_phase_ = 2.0f;
    }

    void Start(
            int32_t pre_delay,
            int32_t buffer_size,
            int32_t start,
            int32_t width,
            int32_t phase_increment,
            float window_shape,
            float gain_l,
            float gain_r,
            GrainQuality recommended_quality) {
        pre_delay_ = pre_delay;
        width_ = width;
        first_sample_ = (start + buffer_size) % buffer_size;
        phase_increment_ = phase_increment;
        phase_ = 0;
        envelope_phase_ = 0.0f;
        envelope_phase_increment_ = 2.0f / static_cast<float>(width);
        if (window_shape >= 0.5f) {
            envelope_smoothness_ = (window_shape - 0.5f) * 2.0f;
            envelope_slope_ = 0.0f;
        } else {
            envelope_smoothness_ = 0.0f;
            envelope_slope_ = 0.5f / (window_shape + 0.01f);
        }
        active_ = true;
        gain_l_ = gain_l;
        gain_r_ = gain_r;
        recommended_quality_ = recommended_quality;
    }

    template<bool use_lut_for_envelope, GrainQuality quality>
    inline void RenderEnvelope(float* destination, size_t size) {
        const float increment = envelope_phase_increment_;
        const float smoothness = envelope_smoothness_;
        const float slope = envelope_slope_;

        float phase = envelope_phase_;
        while (size--) {
            float gain = phase;
            gain = gain >= 1.0f ? 2.0f - gain : gain;  // Triangle envelope
            if (use_lut_for_envelope) {
                if (quality == GRAIN_QUALITY_HIGH) {
                    float window = stmlib::Interpolate(lut_window, gain, 4096.0f);
                    gain += smoothness * (window - gain);
                }
            } else {
                if (quality >= GRAIN_QUALITY_MEDIUM) {
                    gain *= slope;
                    if (gain >= 1.0f) gain = 1.0f;
                }
            }
            phase += increment;
            if (phase >= 2.0f) {
                *destination = -1.0f;  // Signal end of grain
                break;
            }
            *destination++ = gain;
        }
        envelope_phase_ = phase;
    }

    // Simplified OverlapAdd that works with our buffer format
    // buffer is interleaved stereo int16_t (channels = 2) or mono (1, the
    // looper's mono tracks), we accumulate to float stereo output
    inline void OverlapAdd(
            const int16_t* buffer,
            int32_t buffer_size,
            float* destination,
            float* envelope,
            size_t size,
            int channels = 2) {
        if (!active_) {
            return;
        }

        // Handle pre-delay
        while (pre_delay_ && size) {
            destination += 2;
            --size;
            --pre_delay_;
        }

        // Pre-render envelope
        if (envelope_smoothness_ == 0.0f) {
            RenderEnvelope<false, GRAIN_QUALITY_MEDIUM>(envelope, size);
        } else {
            RenderEnvelope<true, GRAIN_QUALITY_HIGH>(envelope, size);
        }

        const int32_t phase_increment = phase_increment_;
        const int32_t first_sample = first_sample_;
        const float gain_l = gain_l_;
        const float gain_r = gain_r_;
        int32_t phase = phase_;

        while (size--) {
            int32_t sample_index = first_sample + (phase >> 16);
            // Wrap to buffer size
            while (sample_index >= buffer_size) sample_index -= buffer_size;
            while (sample_index < 0) sample_index += buffer_size;

            float gain = *envelope++;
            if (gain == -1.0f) {
                active_ = false;
                break;
            }

            // Read samples with linear interpolation
            const int32_t next = sample_index + 1 < buffer_size ? sample_index + 1 : 0;
            float frac = static_cast<float>(phase & 0xFFFF) / 65536.0f;

            float l0, r0, l1, r1;
            if (channels == 1) {
                l0 = r0 = buffer[sample_index] / 32768.0f;
                l1 = r1 = buffer[next] / 32768.0f;
            } else {
                // A stereo frame is one 32-bit word (left low, little-endian):
                // half the reads of the uncached SDRAM.
                const uint32_t* w = reinterpret_cast<const uint32_t*>(buffer);
                const uint32_t a = w[sample_index], b = w[next];
                l0 = int16_t(a & 0xFFFFu) / 32768.0f;
                r0 = int16_t(a >> 16) / 32768.0f;
                l1 = int16_t(b & 0xFFFFu) / 32768.0f;
                r1 = int16_t(b >> 16) / 32768.0f;
            }

            float l = l0 + (l1 - l0) * frac;
            float r = r0 + (r1 - r0) * frac;

            // Apply envelope and panning
            l *= gain;
            r *= gain;
            *destination++ += l * gain_l + r * (1.0f - gain_r);
            *destination++ += r * gain_r + l * (1.0f - gain_l);

            phase += phase_increment;
        }
        phase_ = phase;
    }

    inline bool active() const { return active_; }
    inline GrainQuality recommended_quality() const { return recommended_quality_; }

    // Accessors for UI visualization
    inline int32_t firstSample() const { return first_sample_; }
    inline int32_t width() const { return width_; }
    inline float envelopePhase() const { return envelope_phase_; }
    inline float gainL() const { return gain_l_; }
    inline float gainR() const { return gain_r_; }

private:
    int32_t first_sample_;
    int32_t width_;
    int32_t phase_;
    int32_t phase_increment_;
    int32_t pre_delay_;

    float envelope_smoothness_;
    float envelope_slope_;
    float envelope_phase_;
    float envelope_phase_increment_;

    float gain_l_;
    float gain_r_;

    bool active_;
    GrainQuality recommended_quality_;
};

}  // namespace clouds
