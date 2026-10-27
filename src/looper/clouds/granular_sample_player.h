// Granular playback of audio stored in a buffer.
// Ported from Mutable Instruments Clouds (MIT License)
// Copyright 2014 Emilie Gillet.
#pragma once

#include <cstdint>
#include <algorithm>
#include <cstring>

#include "looper/clouds/stmlib_compat.h"
#include "looper/clouds/grain.h"
#include "looper/clouds/resources.h"

namespace clouds {

static constexpr int32_t kMaxNumGrains = 40;
static constexpr int32_t kMaxBlockSize = 128;  // the looper's audio block (CloudsFx runs ~93 at 32 kHz)
static constexpr int32_t kMaxVisualGrains = 16;  // Subset for UI display

// Grain state for UI visualization (lightweight, thread-safe read)
struct GrainVisual {
    float position;     // 0-1, normalized position in buffer
    float phase;        // 0-1, progress through grain envelope (0=start, 1=end)
    float pan;          // 0-1, stereo position (0=left, 0.5=center, 1=right)
    float size;         // 0-1, normalized grain size
    bool active;        // true if grain is sounding
};

// Parameters structure matching Clouds
struct Parameters {
    float position;
    float size;
    float pitch;
    float density;
    float texture;
    float dry_wet;
    float stereo_spread;
    float pos_spread;
    float feedback;

    bool freeze;
    bool trigger;
    bool gate;

    struct Granular {
        float overlap;
        float window_shape;
        float stereo_spread;
        bool use_deterministic_seed;
    } granular;
};

class GranularSamplePlayer {
public:
    GranularSamplePlayer() { }
    ~GranularSamplePlayer() { }

    void Init(int32_t num_channels, int32_t max_num_grains) {
        max_num_grains_ = max_num_grains;
        num_midfi_grains_ = 3 * max_num_grains / 4;
        gain_normalization_ = 1.0f;
        for (int32_t i = 0; i < kMaxNumGrains; ++i) {
            grains_[i].Init();
        }
        num_grains_ = 0.0f;
        num_channels_ = num_channels;
        grain_size_hint_ = 1024.0f;
        grain_rate_phasor_ = 0.0f;
    }

    // Grain sizes are Clouds' table (32 kHz samples) times this: at another
    // sample rate, rate / 32000 keeps their lengths (the looper runs at 44.1 kHz).
    void set_size_scale(float s) { size_scale_ = s; }

    // Play granular output from buffer
    // buffer: interleaved stereo int16_t
    // buffer_size: number of stereo frames
    // buffer_head: current write position
    void Play(
            const int16_t* buffer,
            int32_t buffer_size,
            int32_t buffer_head,
            const Parameters& parameters,
            float* out,
            size_t size) {

        // Density controls overlap with cubic response and deadband
        float overlap = parameters.granular.overlap;
        overlap = overlap * overlap * overlap;  // Cubic curve!
        float target_num_grains = max_num_grains_ * overlap;
        float p = target_num_grains / static_cast<float>(grain_size_hint_);
        float space_between_grains = grain_size_hint_ / (target_num_grains + 0.001f);

        if (parameters.granular.use_deterministic_seed) {
            p = -1.0f;  // Disable probabilistic
        } else {
            grain_rate_phasor_ = -1000.0f;  // Disable deterministic
        }

        // Build list of available grains
        int32_t num_available_grains = FillAvailableGrainsList();

        // Try to schedule new grains
        bool seed_trigger = parameters.trigger;
        for (size_t t = 0; t < size; ++t) {
            grain_rate_phasor_ += 1.0f;
            bool seed_probabilistic = stmlib::Random::GetFloat() < p
                    && target_num_grains > num_grains_;
            bool seed_deterministic = grain_rate_phasor_ >= space_between_grains;
            bool seed = seed_probabilistic || seed_deterministic || seed_trigger;

            if (num_available_grains && seed) {
                --num_available_grains;
                int32_t index = available_grains_[num_available_grains];
                GrainQuality quality;
                if (num_available_grains < num_midfi_grains_) {
                    quality = GRAIN_QUALITY_MEDIUM;
                } else {
                    quality = GRAIN_QUALITY_HIGH;
                }

                Grain* g = &grains_[index];
                ScheduleGrain(
                        g,
                        parameters,
                        t,
                        buffer_size,
                        buffer_head - size + t,
                        quality);
                grain_rate_phasor_ = 0.0f;
                seed_trigger = false;
            }
        }

        // Clear output and overlap grains
        std::fill(&out[0], &out[size * 2], 0.0f);
        float* e = envelope_buffer_;
        for (int32_t i = 0; i < max_num_grains_; ++i) {
            Grain* g = &grains_[i];
            if (g->active()) {
                g->OverlapAdd(buffer, buffer_size, out, e, size);
            }
        }

        // Compute normalization factor (1/sqrt(n-1) when n > 2)
        int32_t active_grains = max_num_grains_ - num_available_grains;
        SLOPE(num_grains_, static_cast<float>(active_grains), 0.9f, 0.2f);

        float gain_normalization = num_grains_ > 2.0f
                ? stmlib::fast_rsqrt_carmack(num_grains_ - 1.0f)
                : 1.0f;
        float window_gain = 1.0f + 2.0f * parameters.granular.window_shape;
        CONSTRAIN(window_gain, 1.0f, 2.0f);
        gain_normalization *= stmlib::Crossfade(
                1.0f, window_gain, parameters.granular.overlap);

        // Apply gain normalization
        for (size_t t = 0; t < size; ++t) {
            ONE_POLE(gain_normalization_, gain_normalization, 0.01f)
            *out++ *= gain_normalization_;
            *out++ *= gain_normalization_;
        }
    }

    // Extract grain states for UI visualization
    // Returns number of active grains written (up to maxOut)
    int GetGrainStates(GrainVisual* out, int maxOut, int32_t bufferSize) const {
        int count = 0;
        for (int32_t i = 0; i < max_num_grains_ && count < maxOut; ++i) {
            const Grain& g = grains_[i];
            if (g.active()) {
                GrainVisual& v = out[count++];
                v.active = true;
                // Normalize position to 0-1 range
                int32_t pos = g.firstSample();
                while (pos < 0) pos += bufferSize;
                while (pos >= bufferSize) pos -= bufferSize;
                v.position = float(pos) / float(bufferSize);
                // Envelope phase 0-2 maps to 0-1
                v.phase = g.envelopePhase() * 0.5f;
                if (v.phase > 1.0f) v.phase = 1.0f;
                // Derive pan from gain_l/gain_r
                float gl = g.gainL();
                float gr = g.gainR();
                v.pan = gr / (gl + gr + 0.001f);
                // Normalize size
                v.size = float(g.width()) / float(bufferSize);
                if (v.size > 1.0f) v.size = 1.0f;
            }
        }
        return count;
    }

    int GetActiveGrainCount() const {
        int count = 0;
        for (int32_t i = 0; i < max_num_grains_; ++i) {
            if (grains_[i].active()) ++count;
        }
        return count;
    }

private:
    int32_t FillAvailableGrainsList() {
        int32_t num_available_grains = 0;
        for (int32_t i = 0; i < max_num_grains_; ++i) {
            if (!grains_[i].active()) {
                available_grains_[num_available_grains] = i;
                ++num_available_grains;
            }
        }
        return num_available_grains;
    }

    void ScheduleGrain(
            Grain* grain,
            const Parameters& parameters,
            int32_t pre_delay,
            int32_t buffer_size,
            int32_t buffer_head,
            GrainQuality quality) {

        // Randomize position based on pos_spread parameter
        float position = parameters.position;
        if (parameters.pos_spread > 0.0f) {
            float spread = parameters.pos_spread * (stmlib::Random::GetFloat() - 0.5f);
            position = position + spread;
            // Clamp to valid range
            if (position < 0.0f) position = 0.0f;
            if (position > 1.0f) position = 1.0f;
        }
        float pitch = parameters.pitch;
        float window_shape = parameters.granular.window_shape;
        float grain_size = stmlib::Interpolate(lut_grain_size, parameters.size, 256.0f) * size_scale_;
        float pitch_ratio = SemitonesToRatio(pitch);
        float inv_pitch_ratio = SemitonesToRatio(-pitch);

        // Randomize pan position
        float pan = 0.5f + parameters.stereo_spread * (stmlib::Random::GetFloat() - 0.5f);
        float gain_l, gain_r;
        if (num_channels_ == 1) {
            gain_l = stmlib::Interpolate(lut_sin, pan, 256.0f);
            gain_r = stmlib::Interpolate(lut_sin, 1.0f - pan, 256.0f);
        } else {
            if (pan < 0.5f) {
                gain_l = 1.0f;
                gain_r = 2.0f * pan;
            } else {
                gain_r = 1.0f;
                gain_l = 2.0f * (1.0f - pan);
            }
        }

        // Constrain grain size when pitching up
        if (pitch_ratio > 1.0f) {
            grain_size = std::min(grain_size, buffer_size * 0.25f * inv_pitch_ratio);
        }

        float eaten_by_play_head = grain_size * pitch_ratio;
        float eaten_by_recording_head = grain_size;

        float available = static_cast<float>(buffer_size);
        available -= eaten_by_play_head;
        available -= eaten_by_recording_head;
        if (available < 0.0f) available = 0.0f;

        int32_t size = static_cast<int32_t>(grain_size) & ~1;
        int32_t start = buffer_head - static_cast<int32_t>(
                position * available + eaten_by_play_head);

        grain->Start(
                pre_delay,
                buffer_size,
                start,
                size,
                static_cast<uint32_t>(pitch_ratio * 65536.0f),
                window_shape,
                gain_l,
                gain_r,
                quality);

        ONE_POLE(grain_size_hint_, grain_size, 0.1f);
    }

    int32_t max_num_grains_;
    int32_t num_midfi_grains_;
    int32_t num_channels_;

    float num_grains_;
    float gain_normalization_;
    float grain_size_hint_;
    float grain_rate_phasor_;
    float size_scale_ = 1.0f;

    Grain grains_[kMaxNumGrains];
    int32_t available_grains_[kMaxNumGrains];
    float envelope_buffer_[kMaxBlockSize];
};

}  // namespace clouds
