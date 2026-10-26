// OpenTracker -- Sample Recorder input gain, stock 1.9.2 scale (spec 22 §3,
// 0x56640). The per-source gain 0..100 is analog, in the codec; nothing is
// applied digitally afterwards. Integer math as stock (gain 50 -> level 7).
#pragma once

#include <stdint.h>

namespace ot {
namespace input_gain {

// Line in (source 0) and Radio (source 1): lineInLevel(gain * 15 / 100),
// CHIP_ANA_ADC_CTRL nibble 0..15 = 0..+22.5 dB in 1.5 dB steps.
constexpr uint8_t lineLevel(uint8_t gain) { return uint8_t((gain > 100 ? 100 : gain) * 15 / 100); }

// Mic low gain (source 2): micGain(gain * 25 / 100 dB); mic high gain
// (source 3): micGain(gain * 44 / 100 dB) (spec 01 §5.1 micGain split).
constexpr uint8_t micDb(uint8_t source, uint8_t gain) {
    return uint8_t((gain > 100 ? 100 : gain) * (source == 3 ? 44 : 25) / 100);
}

// spec 01 §5.5 factory audio test levels (not the recorder scale): line in at
// lineInLevel 15, mic at micGain 20 dB. The recorder gains that give them.
constexpr uint8_t kFactoryLineGain = 100;   // lineLevel(100) = 15
constexpr uint8_t kFactoryMicGain = 80;     // micDb(2, 80) = 20

static_assert(lineLevel(100) == 15 && lineLevel(50) == 7 && lineLevel(0) == 0, "spec 22 §3");
static_assert(micDb(2, 100) == 25 && micDb(3, 100) == 44, "spec 22 §3");
static_assert(micDb(2, kFactoryMicGain) == 20, "spec 01 §5.5");

}  // namespace input_gain
}  // namespace ot
