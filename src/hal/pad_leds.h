// OpenTracker -- IS31FL3731 pad backlight implementing ILeds (spec 01 §7.3, spec 05 §2.1.3).
// The pads are single-colour: setPad() uses max(r, g, b) as the brightness.
#pragma once

#include <stdint.h>
#include "hal_api.h"
#include "board.h"

namespace ot {
namespace hal {

class PadLeds final : public ILeds {
public:
    bool init();
    bool ok() const { return ok_; }
    void service();                 // deferred flush (>= 30 ms apart) + chip watchdog
    void setLevel(uint8_t pad, uint8_t level31);   // native 0..31 level (gamma mapped)
    void shutdown();                // all off + software shutdown

    // ILeds
    uint8_t padCount() const override { return board::kPadCount; }
    void setPad(uint8_t pad, uint8_t r, uint8_t g, uint8_t b) override;
    void clear() override;
    void show() override;

private:
    bool flush();
    bool writeFunction(uint8_t reg, uint8_t val);

    uint8_t pwm_[144] = {};
    bool dirty_ = false;
    bool ok_ = false;
    uint32_t lastFlush_ = 0, wdLast_ = 0;
};

}  // namespace hal
}  // namespace ot
