// OpenTracker -- polled I2C master for the Kinetis I2C modules (spec 01 §0, §7.5).
#pragma once

#include <stdint.h>
#include "board.h"

namespace ot {
namespace hal {

class I2cBus {
public:
    // index: Kinetis module number (0 or 2 on this board, spec 01 §11).
    constexpr explicit I2cBus(uint8_t index) : index_(index) {}

    void begin();               // clock gate, pin mux, 400 kHz class divider
    void recover();             // spec 01 §3.2: SCL bit-bang (16 toggles) + re-init

    // All return true on success (every byte ACKed, no timeout).
    bool write(uint8_t addr, const uint8_t* data, uint32_t len);
    bool read(uint8_t addr, uint8_t* data, uint32_t len);
    bool writeRead(uint8_t addr, const uint8_t* w, uint32_t wlen, uint8_t* r, uint32_t rlen);
    bool probe(uint8_t addr);   // address-only write, true on ACK

    // Convenience for 8-bit register devices (TCA8418, IS31FL3731).
    bool writeReg8(uint8_t addr, uint8_t reg, uint8_t val) {
        uint8_t b[2] = {reg, val};
        return write(addr, b, 2);
    }
    bool readReg8(uint8_t addr, uint8_t reg, uint8_t& val) {
        return writeRead(addr, &reg, 1, &val, 1);
    }

    // Bus faults: timeouts + lost arbitration (not NAKs). Shown as "i2c errors".
    uint32_t errorCount() const { return timeouts_ + arbLost_; }
    uint32_t nakCount() const { return naks_; }          // device did not ACK
    uint32_t timeoutCount() const { return timeouts_; }
    uint32_t arbLostCount() const { return arbLost_; }
    uint32_t recoveryCount() const { return recoveries_; }
    uint8_t index() const { return index_; }

    enum Result : uint8_t { kOk, kNak, kTimeout, kArbLost };

private:
    struct Regs;
    Regs* regs() const;
    Result waitIf();
    Result start(uint8_t addrRw, bool repeated);
    void stop();
    Result txBytes(const uint8_t* data, uint32_t len);
    Result rxBytes(uint8_t* data, uint32_t len);
    bool fail(Result why);
    bool linesStuck();
    void maybeRecover();

    uint8_t index_;
    uint32_t naks_ = 0, timeouts_ = 0, arbLost_ = 0, recoveries_ = 0;
    uint32_t busySinceMs_ = 0;   // 0 = bus not seen busy at a start
};

// A polled wait has expired only when wall-clock time ran out AND the loop itself
// polled kI2cMinPolls times; the caller then re-reads the flag once more. Interrupt
// time (audio DMA ISR) cannot make a wait expire on its own (spec 01 §7.5 timeout).
constexpr bool i2cWaitExpired(uint32_t elapsedUs, uint32_t polls, uint32_t timeoutUs) {
    return polls >= board::kI2cMinPolls && elapsedUs > timeoutUs;
}

I2cBus& i2c0();
I2cBus& i2c2();
I2cBus& padBus();   // bus carrying the pad keypad + pad LEDs (board::kPadBusIndex)

}  // namespace hal
}  // namespace ot
