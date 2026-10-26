// OpenTracker -- IS31FL3731 pad LEDs (spec 01 §7.3). Register map: ISSI IS31FL3731 datasheet.
#include "pad_leds.h"

#include <Arduino.h>
#include <string.h>
#include "i2c_bus.h"

namespace ot {
namespace hal {

namespace {
constexpr uint8_t REG_PAGE = 0xFD, PAGE_FUNCTION = 0x0B, PAGE_FRAME0 = 0x00;
constexpr uint8_t FN_MODE = 0x00, FN_FRAME = 0x01, FN_AUDIOSYNC = 0x06, FN_SHUTDOWN = 0x0A;
// spec 01 §7.3 gamma table (level 0..31 -> PWM).
constexpr uint8_t kGamma[32] = {0, 1, 2, 4, 6, 10, 13, 18, 22, 28, 33, 39, 46, 53, 61, 69,
                                78, 86, 96, 106, 116, 126, 138, 149, 161, 173, 186, 199, 212, 226, 240, 255};
// spec 01 §7.3: position = idx + (idx/12)*4 (16 positions per matrix row, 12 used).
inline uint8_t position(uint8_t pad) { return uint8_t(pad + (pad / 12) * 4); }
I2cBus& bus() { return padBus(); }
}  // namespace

bool PadLeds::writeFunction(uint8_t reg, uint8_t val) {
    return bus().writeReg8(board::kAddrPadLeds, REG_PAGE, PAGE_FUNCTION) &&
           bus().writeReg8(board::kAddrPadLeds, reg, val);
}

bool PadLeds::init() {
    // spec 01 §7.3 init
    bool ok = writeFunction(FN_SHUTDOWN, 0);
    delay(10);
    ok = writeFunction(FN_SHUTDOWN, 1) && ok;
    ok = bus().writeReg8(board::kAddrPadLeds, FN_MODE, 0) && ok;      // picture mode
    ok = bus().writeReg8(board::kAddrPadLeds, FN_FRAME, 0) && ok;     // display frame 0
    ok = bus().writeReg8(board::kAddrPadLeds, FN_AUDIOSYNC, 0) && ok;
    memset(pwm_, 0, sizeof(pwm_));
    ok_ = ok;
    ok = flush() && ok;
    ok_ = ok;
    lastFlush_ = wdLast_ = millis();
    return ok;
}

bool PadLeds::flush() {
    // spec 01 §7.3 flush: one burst from reg 0x00 = 18 enable, 18 blink, 144 PWM.
    uint8_t buf[1 + 18 + 18 + 144];
    buf[0] = 0x00;
    uint8_t* en = buf + 1;
    memset(en, 0, 36);
    for (uint8_t pad = 0; pad < board::kPadCount; pad++) {
        uint8_t p = position(pad);
        en[p >> 3] |= uint8_t(1u << (p & 7));
    }
    memcpy(buf + 37, pwm_, 144);
    bool ok = bus().writeReg8(board::kAddrPadLeds, REG_PAGE, PAGE_FRAME0) &&
              bus().write(board::kAddrPadLeds, buf, sizeof(buf));
    lastFlush_ = millis();
    if (ok) dirty_ = false;
    return ok;
}

void PadLeds::setLevel(uint8_t pad, uint8_t level) {
    if (pad >= board::kPadCount) return;
    if (level > 31) level = 31;
    uint8_t v = kGamma[level];
    uint8_t& slot = pwm_[position(pad)];
    if (slot != v) { slot = v; dirty_ = true; }
}

void PadLeds::setPad(uint8_t pad, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t m = r > g ? r : g;
    if (b > m) m = b;
    setLevel(pad, uint8_t(m >> 3));
}

void PadLeds::clear() {
    for (uint8_t i = 0; i < board::kPadCount; i++) setLevel(i, 0);
}

void PadLeds::show() {
    if (ok_ && dirty_ && millis() - lastFlush_ >= board::kLedFlushMinMs) flush();
}

void PadLeds::service() {
    uint32_t now = millis();
    if (ok_ && dirty_ && now - lastFlush_ >= board::kLedFlushMinMs) flush();
    if (ok_ && now - wdLast_ >= 1000) {
        // spec 01 §7.3 watchdog: shutdown register must read 1, else recover + re-init.
        wdLast_ = now;
        uint8_t v = 0;
        if (!(bus().writeReg8(board::kAddrPadLeds, REG_PAGE, PAGE_FUNCTION) &&
              bus().readReg8(board::kAddrPadLeds, FN_SHUTDOWN, v)) || v != 1) {
            bus().recover();
            uint8_t keep[144];
            memcpy(keep, pwm_, sizeof(keep));
            init();
            memcpy(pwm_, keep, sizeof(keep));
            dirty_ = true;
        }
    }
}

void PadLeds::shutdown() {
    memset(pwm_, 0, sizeof(pwm_));
    flush();
    writeFunction(FN_SHUTDOWN, 0);
}

}  // namespace hal
}  // namespace ot
