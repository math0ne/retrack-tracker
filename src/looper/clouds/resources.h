// Lookup tables for Clouds granular synthesis.
// Generated from Mutable Instruments Clouds (MIT License)
// Copyright 2014 Emilie Gillet.
#pragma once

#include <cstdint>

namespace clouds {

// Grain size: 257 entries, exponential from 1024 to 16384 samples
// At 32kHz: 32ms to 512ms
extern const float lut_grain_size[257];

// Hanning window: 4097 entries for envelope lookup
extern const float lut_window[4097];

// Sine table: 257 entries for panning (sin curve)
extern const float lut_sin[257];

// Pitch ratio tables for SemitonesToRatio
extern const float lut_pitch_ratio_high[257];
extern const float lut_pitch_ratio_low[257];

// Helper function for semitones to ratio conversion
inline float SemitonesToRatio(float semitones) {
    float pitch = semitones + 128.0f;
    int32_t pitch_integral = static_cast<int32_t>(pitch);
    float pitch_fractional = pitch - static_cast<float>(pitch_integral);
    if (pitch_integral < 0) pitch_integral = 0;
    if (pitch_integral > 255) pitch_integral = 255;
    int32_t frac_index = static_cast<int32_t>(pitch_fractional * 256.0f);
    if (frac_index > 255) frac_index = 255;
    return lut_pitch_ratio_high[pitch_integral] * lut_pitch_ratio_low[frac_index];
}

}  // namespace clouds

