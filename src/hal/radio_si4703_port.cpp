// OpenTracker -- device glue of the Si4703 driver (radio_si4703.h): I2C0 at
// 0x10, the reset pins and the diag log (spec 22 §2.1, §2.2).
#include "radio_si4703.h"

#include <Arduino.h>
#include "board.h"
#include "diag.h"
#include "i2c_bus.h"
#include "k66.h"

namespace ot {
namespace hal {

namespace {
bool portRead(uint8_t* data, uint32_t n) { return i2c0().read(board::kAddrSi4703, data, n); }
bool portWrite(const uint8_t* data, uint32_t n) { return i2c0().write(board::kAddrSi4703, data, n); }
uint32_t portMillis() { return millis(); }

void portReset() {
    // spec 22 §2.2 steps 2-7: RST_N and SDIO (PTD9 = I2C0 SDA) as GPIO outputs,
    // both LOW, 1 ms, RST_N HIGH (SDIO LOW at this edge selects the 2-wire
    // bus; SEN is pulled up on the chip), 1 ms, then a plain I2C0 begin (pins
    // back to the I2C mux; no SCL bit-bang). SCLK stays in the I2C mux. Only
    // called from the main loop, so no other I2C0 transfer (codec, button
    // keypad) can be in flight.
    gpioOutput(board::kPinRadioReset, false);
    gpioOutput(board::kI2c0Sda, false);
    delay(1);
    gpioWrite(board::kPinRadioReset, true);
    delay(1);
    gpio(board::kI2c0Sda).PDDR &= ~(1u << board::kI2c0Sda.bit);
    i2c0().begin();
}

Si4703Radio::Port devicePort() {
    Si4703Radio::Port p;
    p.read = &portRead;
    p.write = &portWrite;
    p.millis = &portMillis;
    p.resetPulse = &portReset;
    p.log = &diagLogf;
    return p;
}
}  // namespace

Si4703Radio& radio() {
    static Si4703Radio r;
    static bool init = false;
    if (!init) {
        init = true;
        r.setPort(devicePort());
    }
    return r;
}

}  // namespace hal
}  // namespace ot
