// OpenTracker -- front panel input: two TCA8418 keypad scanners and the FTM2
// quadrature encoder, implementing IInput (spec 01 §7.1, §7.2, §7.4; spec 05 §2.1).
#pragma once

#include <stdint.h>
#include "hal_api.h"
#include "board.h"

namespace ot {
namespace hal {

class I2cBus;

class PanelInput final : public IInput {
public:
    bool init();                 // both keypads + encoder
    void service();              // read keypads when /INT is low, encoder, hold timers
    bool poll(InputEvent& ev) override;   // also calls service()

    // Queue a button event from another source (the power switch, board.cpp).
    void injectButton(bool down, uint8_t id) {
        push(down ? InputEvent::kButtonDown : InputEvent::kButtonUp, id);
    }

    bool buttonsOk() const { return buttonsOk_; }
    bool padsOk() const { return padsOk_; }
    bool buttonDown(uint8_t id) const { return id < board::kButtonCount && (buttons_ >> id) & 1; }
    bool padDown(uint8_t id) const { return id < board::kPadCount && (pads_ >> id) & 1; }
    int32_t encoderTotal() const { return encTotal_; }
    uint8_t lastButtonCode() const { return lastButtonCode_; }   // raw TCA code, bit7 = press
    uint8_t lastPadCode() const { return lastPadCode_; }

private:
    bool initChip(I2cBus& bus, uint8_t gpio3);
    void readChip(I2cBus& bus, bool pads);
    void push(InputEvent::Type t, uint8_t id, int8_t delta = 0);
    void serviceEncoder(uint32_t now);
    void serviceHolds(uint32_t now);
    void watchdog(uint32_t now);

    static constexpr uint32_t kQueue = 64;
    InputEvent q_[kQueue];
    uint32_t qHead_ = 0, qTail_ = 0;

    uint64_t buttons_ = 0;
    uint64_t pads_ = 0;
    uint32_t holdNext_[board::kButtonCount] = {};
    uint8_t holdLevel_[board::kButtonCount] = {};
    bool buttonsOk_ = false, padsOk_ = false;
    uint8_t lastButtonCode_ = 0, lastPadCode_ = 0;
    int16_t encRemainder_ = 0;
    uint8_t encLastDir_ = 0;
    int32_t encTotal_ = 0;
    uint32_t encLast_ = 0, wdLast_ = 0;
    bool inService_ = false;
};

}  // namespace hal
}  // namespace ot
