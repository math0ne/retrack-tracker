// OpenTracker -- pure clock/divider arithmetic for the MK66 drivers. No register
// access, no Arduino headers: the drivers use these in static_asserts and the
// host tests (tests/hw_math_test.cpp) check them against the spec 01 values.
#pragma once

#include <stdint.h>

namespace ot {
namespace hal {
namespace hw {

// K66 RM, I2C chapter, "I2C divider and hold values" table: ICR -> SCL divider.
constexpr uint16_t kI2cSclDivider[64] = {
    20,   22,   24,   26,   28,   30,   34,   40,   28,   32,   36,   40,   44,   48,   56,   68,
    48,   56,   64,   72,   80,   88,   104,  128,  80,   96,   112,  128,  144,  160,  192,  240,
    160,  192,  224,  256,  288,  320,  384,  480,  320,  384,  448,  512,  576,  640,  768,  960,
    640,  768,  896,  1024, 1152, 1280, 1536, 1920, 1280, 1536, 1792, 2048, 2304, 2560, 3072, 3840};
// I2Cx_F: MULT bits 7:6 (x1/x2/x4), ICR bits 5:0. SCL = bus / (mul * SCL divider).
constexpr uint32_t i2cSclHz(uint32_t busHz, uint8_t f) {
    return busHz / ((1u << ((f >> 6) & 3u)) * kI2cSclDivider[f & 0x3Fu]);
}

// K66 RM, SPI chapter, CTARn: SCK = fsys / PBR * (1 + DBR) / BR.
constexpr uint32_t spiSckHz(uint32_t busHz, uint32_t ctar) {
    constexpr uint8_t pbr[4] = {2, 3, 5, 7};
    constexpr uint16_t br[16] = {2, 4, 6, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192,
                                 16384, 32768};
    return busHz / pbr[(ctar >> 16) & 3u] * (1u + (ctar >> 31)) / br[ctar & 0xFu];
}
constexpr uint32_t spiFrameBits(uint32_t ctar) { return ((ctar >> 27) & 0xFu) + 1u; }

// K66 RM, SDHC chapter, SYSCTL: SDCLK = base / (2 * SDCLKFS) / (DVS + 1);
// SDCLKFS = 0 means divide by 1 (only power-of-two values are valid).
constexpr uint32_t sdhcClockHz(uint32_t baseHz, uint32_t sdclkfs, uint32_t dvs) {
    return baseHz / (sdclkfs ? 2u * sdclkfs : 1u) / (dvs + 1u);
}

// K66 RM, SDRAMC chapter, CTRL[RC]: refresh interval = (RC + 1) * 16 bus clocks.
// spec 01 §2: RC = periodNs * busMHz / 16000 - 1 (15625 ns at 90 MHz -> 86).
constexpr uint32_t sdramRefreshCount(uint32_t busHz, uint32_t periodNs) {
    return uint32_t(uint64_t(periodNs) * (busHz / 1000u) / 16000000u) - 1u;
}
constexpr uint32_t sdramRefreshPeriodNs(uint32_t busHz, uint32_t rc) {
    return uint32_t(uint64_t(rc + 1u) * 16u * 1000000000u / busHz);
}

// K66 RM, UART chapter: baud = module clock / (16 * (SBR + BRFA/32)). UART4 is
// clocked from the bus clock (spec 01 §8). Divisor in 1/32 units (Teensy BAUD2DIV3).
constexpr uint32_t uartDivisor32(uint32_t clockHz, uint32_t baud) {
    return (clockHz * 2u + baud / 2u) / baud;
}
constexpr uint32_t uartBaudFromDivisor32(uint32_t clockHz, uint32_t div32) {
    return uint32_t(uint64_t(clockHz) * 2u / div32);
}

// K66 RM, I2S/SAI chapter: MCLK = in * (FRACT + 1) / (DIVIDE + 1); TCR2: BCLK =
// MCLK / ((DIV + 1) * 2).
constexpr uint32_t i2sMclkHz(uint32_t inHz, uint32_t mdr) {
    return uint32_t(uint64_t(inHz) * (((mdr >> 12) & 0xFFu) + 1u) / ((mdr & 0xFFFu) + 1u));
}
constexpr uint32_t i2sBclkHz(uint32_t mclkHz, uint32_t tcr2) {
    return mclkHz / (((tcr2 & 0xFFu) + 1u) * 2u);
}

// FT81x datasheet, "Display timing": frame rate = sysclk / PCLK / HCYCLE / VCYCLE.
constexpr uint32_t ft81xFrameMilliHz(uint32_t sysHz, uint32_t pclkDiv, uint32_t hcycle,
                                     uint32_t vcycle) {
    return uint32_t(uint64_t(sysHz) * 1000u / pclkDiv / hcycle / vcycle);
}

// ---- status LED blink codes (docs/HARDWARE_TESTING.md) ----------------------
// failMask bit n set -> code n+1: n pulses of 250 ms on / 250 ms off, then a
// 1500 ms gap; codes for all failed subsystems are shown in turn. No failure ->
// 100 ms heartbeat every 2 s.
enum BlinkCode : uint8_t {
    kBlinkDisplay = 1, kBlinkSdram = 2, kBlinkCodec = 3, kBlinkButtons = 4,
    kBlinkPads = 5, kBlinkPadLeds = 6, kBlinkSd = 7, kBlinkClocks = 8,
};
constexpr uint32_t kBlinkOnMs = 250, kBlinkOffMs = 250, kBlinkGapMs = 1500;
constexpr uint32_t blinkCycleMs(uint32_t failMask) {
    uint32_t t = 0;
    for (uint32_t n = 1; n <= 8; n++)
        if (failMask & (1u << (n - 1))) t += n * (kBlinkOnMs + kBlinkOffMs) + kBlinkGapMs;
    return t;
}
constexpr bool statusLedOn(uint32_t failMask, uint32_t tMs) {
    failMask &= 0xFFu;
    if (!failMask) return (tMs % 2000u) < 100u;
    uint32_t t = tMs % blinkCycleMs(failMask);
    for (uint32_t n = 1; n <= 8; n++) {
        if (!(failMask & (1u << (n - 1)))) continue;
        uint32_t len = n * (kBlinkOnMs + kBlinkOffMs) + kBlinkGapMs;
        if (t < len) {
            if (t >= n * (kBlinkOnMs + kBlinkOffMs)) return false;   // gap
            return (t % (kBlinkOnMs + kBlinkOffMs)) < kBlinkOnMs;
        }
        t -= len;
    }
    return false;
}

}  // namespace hw
}  // namespace hal
}  // namespace ot
