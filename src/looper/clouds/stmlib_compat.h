// Compatibility layer for Mutable Instruments stmlib utilities.
// Extracted from https://github.com/pichenettes/stmlib (MIT License)
// Copyright 2012-2014 Emilie Gillet.
#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>

namespace stmlib {

// Macro to extract integral and fractional parts
#define MAKE_INTEGRAL_FRACTIONAL(x) \
    int32_t x ## _integral = static_cast<int32_t>(x); \
    float x ## _fractional = x - static_cast<float>(x ## _integral);

// Table interpolation
inline float Interpolate(const float* table, float index, float size) {
    // The tables hold size + 1 entries: an index of 1.0 (or past it) is the last.
    index *= size;
    if (index >= size) return table[static_cast<int32_t>(size)];
    if (index < 0.0f) return table[0];
    MAKE_INTEGRAL_FRACTIONAL(index)
    float a = table[index_integral];
    float b = table[index_integral + 1];
    return a + (b - a) * index_fractional;
}

// One-pole lowpass filter
#define ONE_POLE(out, in, coefficient) out += (coefficient) * ((in) - out);

// Asymmetric slew
#define SLOPE(out, in, positive, negative) { \
    float error = (in) - out; \
    out += (error > 0 ? positive : negative) * error; \
}

// Constrain to range
#define CONSTRAIN(x, min, max) \
    if (x < min) x = min; \
    else if (x > max) x = max;

// Linear crossfade
inline float Crossfade(float a, float b, float fade) {
    return a + (b - a) * fade;
}

// Soft limiter (warm saturation)
inline float SoftLimit(float x) {
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

// Soft clipper with hard limits at ±1
inline float SoftClip(float x) {
    if (x < -3.0f) return -1.0f;
    else if (x > 3.0f) return 1.0f;
    else return SoftLimit(x);
}

// Clip to 16-bit range
inline int32_t Clip16(int32_t x) {
    if (x < -32768) return -32768;
    else if (x > 32767) return 32767;
    else return x;
}

// Soft convert to 16-bit
inline int16_t SoftConvert(float x) {
    return Clip16(static_cast<int32_t>(SoftLimit(x * 0.5f) * 32768.0f));
}

// Fast inverse square root (Carmack's method)
inline float fast_rsqrt_carmack(float x) {
    float xhalf = 0.5f * x;
    union { float f; int32_t i; } u;
    u.f = x;
    u.i = 0x5f3759df - (u.i >> 1);
    u.f = u.f * (1.5f - xhalf * u.f * u.f);
    return u.f;
}

// Random number generator (Linear Congruential Generator)
class Random {
public:
    static inline uint32_t state() { return rng_state_; }

    static inline void Seed(uint32_t seed) {
        rng_state_ = seed;
    }

    static inline uint32_t GetWord() {
        rng_state_ = rng_state_ * 1664525L + 1013904223L;
        return rng_state_;
    }

    static inline int16_t GetSample() {
        return static_cast<int16_t>(GetWord() >> 16);
    }

    static inline float GetFloat() {
        return static_cast<float>(GetWord()) / 4294967296.0f;
    }

private:
    static uint32_t rng_state_;
};

}  // namespace stmlib
