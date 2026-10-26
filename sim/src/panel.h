// OpenTracker simulator -- front panel drawn around the emulated screen:
// the 8 screen buttons (F1..F8) under the display, the 5x5 key block
// (id = 8 + row*5 + col, spec 05 §2.1.1 key map / spec 06 §0.2 names), Aux and
// Help, the 12x4 pad grid lit from ILeds, and the encoder with its push.
// The physical arrangement is an approximation; ids are the canonical ones
// from src/core/ui/buttons.h.
#pragma once

#include <cstdint>
#include <string>

#include "canvas.h"

namespace sim {

class SimInput;
class SimLeds;

struct ButtonInfo {
    uint8_t id;
    const char* label;   // panel label
    const char* name;    // script name (case-insensitive)
};
// All buttons the panel shows (F1..F8, 0x08..0x22, 0x2b).
const ButtonInfo* buttonTable(int* count);
// Script/CLI name or number ("PLAY", "F1", "0x17", "23") -> id, or -1.
int buttonByName(const char* name);

struct Control {
    enum Kind : uint8_t { kNone, kButton, kPad, kEncoderCcw, kEncoderCw, kEncoderPush } kind = kNone;
    uint8_t id = 0;
};

class Panel {
public:
    static constexpr int kW = 1224, kH = 784;
    static constexpr int kScreenX = 24, kScreenY = 24;

    Control hit(int x, int y) const;
    // Draws everything except the screen contents, then blits `screen` (800x480 ARGB).
    void draw(Canvas& c, const uint32_t* screen, const SimInput& in, const SimLeds& leds, uint64_t latched,
              const std::string& status) const;
};

}  // namespace sim
