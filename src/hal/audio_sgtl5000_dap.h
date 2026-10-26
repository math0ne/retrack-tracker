// OpenTracker -- SGTL5000 Digital Audio Processor (DAP) programming for the
// Master EQ, Bass Boost and Space controls (spec 18 §1, spec 01 §5.1 wrapper).
//
// Header-only and free of Teensy dependencies so the host tests can drive it
// against a fake register map (tests/codec_dsp_test.cpp). The register
// semantics follow the SGTL5000 datasheet; every write that changes a field
// is a read-modify-write of that field only (the wrapper's modify()).
//
// The DAP sits between I2S-in and the DAC, after the MCU mix, so offline
// renders/exports never contain these effects (spec 18 §1.5).
#pragma once

#include <stdint.h>

namespace ot {
namespace hal {
namespace sgtl_dap {

// SGTL5000 register addresses (datasheet register map).
constexpr uint16_t kChipSssCtrl = 0x000A;
constexpr uint16_t kDapControl = 0x0100;
constexpr uint16_t kDapBassEnhance = 0x0104;
constexpr uint16_t kDapBassEnhanceCtrl = 0x0106;
constexpr uint16_t kDapAudioEq = 0x0108;
constexpr uint16_t kDapSurround = 0x010A;
constexpr uint16_t kDapEqBand0 = 0x0116;   // bands at +2b: 0x116..0x11E

constexpr int kBands = 5;                  // 115 / 330 / 990 / 3000 / 9900 Hz
// enhanceBass lr level: stock passes the float 0x3F555555 (spec 18 §1.3).
constexpr float kBassLr = 0.8333333f;

// Register bus: the driver passes its I2C codecRead/codecWrite, tests a fake.
struct Bus {
    bool (*read)(void* ctx, uint16_t reg, uint16_t& val) = nullptr;
    bool (*write)(void* ctx, uint16_t reg, uint16_t val) = nullptr;
    void* ctx = nullptr;
};

// spec 18 §1.2: band gain g (-1..1) -> 7-bit band code, 0x2F = 0 dB, 0.25 dB/step.
inline uint16_t eqBandCode(float g) {
    if (!(g >= -1.0f)) g = -1.0f;          // also catches NaN
    if (g > 1.0f) g = 1.0f;
    float n = g * 48.0f + 0.499f;
    if (n < -47.0f) n = -47.0f;
    if (n > 48.0f) n = 48.0f;
    return uint16_t(n + 47.0f);
}

// wrapper calcVol(n, range): trunc(n*range + 0.499), clamped to range.
inline uint16_t calcVol(float n, uint16_t range) {
    const float v = n * float(range) + 0.499f;
    if (!(v > 0.0f)) return 0;
    const uint16_t u = uint16_t(v);
    return u > range ? range : u;
}

// spec 18 §1.3: Bass Boost 0..100 -> bass level 0.8*v/100 (v > 100 -> 0.8).
// UNVERIFIED [I]: evaluation order (0.8*v)/100 in single precision; checked
// in tests that both orders agree for every v in 0..100.
inline float bassLevel(uint8_t v) {
    if (v > 100) v = 100;
    float b = 0.8f * float(v) / 100.0f;
    if (b < 0.0f) b = 0.0f;
    if (b > 1.0f) b = 1.0f;
    return b;
}

// DAP_BASS_ENHANCE_CTRL value for enhanceBass(lr, bass): bits 13:8 LR_LEVEL,
// bits 6:0 BASS_LEVEL (both inverted levels).
inline uint16_t bassCtrlValue(float lr, float bass) {
    return uint16_t(((63u - calcVol(lr, 63)) << 8) | (127u - calcVol(bass, 127)));
}

class Dap {
public:
    void attach(const Bus& bus) { bus_ = bus; }

    // Read-modify-write of the bits in mask (wrapper modify()).
    bool modify(uint16_t reg, uint16_t val, uint16_t mask) {
        uint16_t old = 0;
        if (!bus_.read || !bus_.read(bus_.ctx, reg, old)) return false;
        return write(reg, uint16_t((old & ~mask) | (val & mask)));
    }
    bool write(uint16_t reg, uint16_t val) { return bus_.write && bus_.write(bus_.ctx, reg, val); }

    // Engine-init DAP sequence (spec 18 §1.1, stock FW:0x1611c):
    //  1. DAP on: DAP_CONTROL = 1, then SSS_CTRL = 0x70 (I2S_IN -> DAP -> DAC);
    //  2. eqSelect(3) (5-band GEQ), then the 5 band gains (flat here; the app
    //     pushes the project values right after, as stock project load does);
    //  3. surround off, bass enhance on (0x104 |= 1), enhanceBass(0.8333, 0, 1, 0).
    // UNVERIFIED [I]: stock writes the band gains from project RAM at this
    // point; on a fresh boot those are the New Project zeros.
    bool init() {
        bool ok = write(kDapControl, 0x0001);
        ok = write(kChipSssCtrl, 0x0070) && ok;
        ok = eqSelect(3) && ok;
        const float flat[kBands] = {0, 0, 0, 0, 0};
        ok = setEqBands(flat) && ok;
        ok = surroundEnable(false) && ok;
        surroundDisabled_ = true;
        ok = modify(kDapBassEnhance, 0x0001, 0x0001) && ok;
        ok = enhanceBass(kBassLr, 0.0f, 1, 0) && ok;
        valid_ = false;   // next apply() writes every control
        return ok;
    }

    // DAP off (spec 01 §5.1): SSS = 0x10 first, then DAP_CONTROL = 0.
    bool disable() {
        bool ok = write(kChipSssCtrl, 0x0010);
        return write(kDapControl, 0x0000) && ok;
    }

    bool eqSelect(uint8_t n) { return modify(kDapAudioEq, n & 3, 0x0003); }

    // spec 18 §1.2 (stock FW:0x70514): EQ mode 3 if not already, then bands.
    bool setEqBands(const float g[kBands]) {
        uint16_t mode = 0;
        bool ok = bus_.read && bus_.read(bus_.ctx, kDapAudioEq, mode);
        if (ok && (mode & 3) != 3) ok = eqSelect(3);
        for (int b = 0; b < kBands; ++b)
            ok = modify(uint16_t(kDapEqBand0 + 2 * b), eqBandCode(g[b]), 0x007F) && ok;
        return ok;
    }

    // enhanceBass(lr, bass, hpfBypass, cutoff): 0x104 bit 8 / bits 6:4, then 0x106.
    bool enhanceBass(float lr, float bass, uint8_t hpfBypass, uint8_t cutoff) {
        bool ok = modify(kDapBassEnhance, uint16_t(((hpfBypass & 1u) << 8) | ((cutoff & 7u) << 4)), 0x0170);
        return modify(kDapBassEnhanceCtrl, bassCtrlValue(lr, bass), 0x3F7F) && ok;
    }

    // spec 18 §1.3 (stock FW:0x7059c). Level 0 leaves the enhancer enabled.
    bool setBassBoost(uint8_t v) { return enhanceBass(kBassLr, bassLevel(v), 1, 0); }

    bool surroundEnable(bool on) { return modify(kDapSurround, on ? 3 : 0, 0x0003); }

    // spec 18 §1.4 (stock FW:0x705e4): 0 = off; 1..8 -> WIDTH 8-v, SELECT 3.
    bool setSpace(uint8_t v) {
        if (v == 0) {
            surroundDisabled_ = true;
            return surroundEnable(false);
        }
        if (v > 8) v = 8;
        bool ok = true;
        if (surroundDisabled_) {
            ok = surroundEnable(true);
            surroundDisabled_ = false;
        }
        const uint16_t width = uint16_t(8 - v), select = 3;
        return modify(kDapSurround, uint16_t(((width & 7u) << 4) | (select & 3u)), 0x0073) && ok;
    }

    // Main-loop sync from the project values: writes only the controls that
    // changed since the last call (all of them after init()). Stock re-applies
    // all three on every Master edit and project load; skipping unchanged
    // controls leaves the same register state.
    void apply(const float eq[kBands], uint8_t bass, uint8_t space) {
        bool eqChanged = !valid_;
        for (int b = 0; b < kBands; ++b) eqChanged = eqChanged || eqBandCode(eq[b]) != eqCode_[b];
        if (eqChanged) {
            setEqBands(eq);
            for (int b = 0; b < kBands; ++b) eqCode_[b] = eqBandCode(eq[b]);
        }
        if (!valid_ || bass != bass_) setBassBoost(bass);
        if (!valid_ || space != space_) setSpace(space);
        bass_ = bass;
        space_ = space;
        valid_ = true;
    }

private:
    Bus bus_;
    bool surroundDisabled_ = true;   // stock flag 0x2000c2b4, ctor value 1
    bool valid_ = false;
    uint16_t eqCode_[kBands] = {0, 0, 0, 0, 0};
    uint8_t bass_ = 0, space_ = 0;
};

}  // namespace sgtl_dap
}  // namespace hal
}  // namespace ot
