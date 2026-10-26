// OpenTracker -- polled I2C master (spec 01 §0 step 3, §3.2 bus recovery, §7.5).
// Register model: NXP K66 reference manual, chapter "Inter-Integrated Circuit (I2C)".
#include "i2c_bus.h"

#include <Arduino.h>
#include "hw_math.h"
#include "k66.h"

namespace ot {
namespace hal {

struct I2cBus::Regs {
    volatile uint8_t A1, F, C1, S, D, C2, FLT, RA, SMB, A2, SLTH, SLTL;
};

namespace {
constexpr uint8_t C1_IICEN = 0x80, C1_MST = 0x20, C1_TX = 0x10, C1_TXAK = 0x08, C1_RSTA = 0x04;
constexpr uint8_t S_BUSY = 0x20, S_ARBL = 0x10, S_IICIF = 0x02, S_RXAK = 0x01;
// ICR 0x1F = SCL divider 240, MULT=0 -> 90 MHz / 240 = 375 kHz (spec 01 says
// 400 kHz; 224 would give 401.8 kHz, slightly over spec). MULT must stay 0 for
// repeated starts (K66 errata e6070).
constexpr uint8_t kFreqDiv = 0x1F;
static_assert(board::kBusHz == 90000000, "I2C divider assumes the 90 MHz bus (spec 01 §1)");
static_assert(hw::i2cSclHz(board::kBusHz, kFreqDiv) <= board::kI2cHz, "I2C SCL above 400 kHz");

struct BusPins { board::Pin scl, sda; uint32_t pcrVal; };
BusPins pinsFor(uint8_t idx) {
    if (idx == 0) return {board::kI2c0Scl, board::kI2c0Sda, board::kI2c0Pcr};
    return {board::kI2c2Scl, board::kI2c2Sda, board::kI2c2Pcr};
}
}  // namespace

I2cBus::Regs* I2cBus::regs() const {
    return reinterpret_cast<Regs*>(index_ == 0 ? 0x40066000u : 0x400E6000u);
}

void I2cBus::begin() {
    if (index_ == 0) SIM_SCGC4 |= SIM_SCGC4_I2C0;
    else SIM_SCGC1 |= SIM_SCGC1_I2C2;
    Regs* r = regs();
    r->C1 = 0;
    BusPins p = pinsFor(index_);
    pcr(p.scl) = p.pcrVal;
    pcr(p.sda) = p.pcrVal;
    r->F = kFreqDiv;
    r->FLT = 4;          // glitch filter (bus clocks)
    r->C2 = 0;
    r->S = S_IICIF | S_ARBL;
    r->C1 = C1_IICEN;
}

void I2cBus::recover() {
    // spec 01 §3.2: SCL switched to GPIO (PCR 0x100), 16 toggles (8 pulses), re-init.
    BusPins p = pinsFor(index_);
    // The module clock gate must be on before any I2C register access (K66 RM
    // SIM SCGCn: a gated peripheral answers with a bus error -> HardFault). The
    // first recover() at boot runs before begin() has set it.
    if (index_ == 0) SIM_SCGC4 |= SIM_SCGC4_I2C0;
    else SIM_SCGC1 |= SIM_SCGC1_I2C2;
    regs()->C1 = 0;
    gpio(p.scl).PSOR = 1u << p.scl.bit;
    gpio(p.scl).PDDR |= 1u << p.scl.bit;
    pcr(p.scl) = 0x100 | 0x20;   // GPIO, open drain so we never fight a slave
    for (int i = 0; i < 16; i++) {
        gpio(p.scl).PTOR = 1u << p.scl.bit;
        delayMicroseconds(5);
    }
    gpio(p.scl).PSOR = 1u << p.scl.bit;
    gpio(p.scl).PDDR &= ~(1u << p.scl.bit);
    begin();
}

bool I2cBus::linesStuck() {
    // UNVERIFIED: K66 GPIO PDIR reflects the pin level while the pin is muxed to
    // the I2C function (digital mux, input buffer on). A line is stuck when it
    // reads low on every sample over ~50 us with no transfer in progress.
    BusPins p = pinsFor(index_);
    for (int i = 0; i < 5; i++) {
        if (gpioRead(p.scl) && gpioRead(p.sda)) return false;
        delayMicroseconds(10);
    }
    return true;
}

void I2cBus::maybeRecover() {
    // Bit-bang recovery (spec 01 §3.2) only for a bus that is really held low;
    // a timeout with idle lines just leaves the module re-initialised.
    if (linesStuck()) {
        recoveries_++;
        recover();
    } else if (regs()->S & S_BUSY) {
        begin();
    }
}

bool I2cBus::fail(Result why) {
    if (why == kNak) naks_++;
    else if (why == kArbLost) arbLost_++;
    else timeouts_++;
    stop();
    if (why != kNak) maybeRecover();
    return false;
}

I2cBus::Result I2cBus::waitIf() {
    // The timeout applies per byte (9 SCL clocks = 24 us at 375 kHz, plus clock
    // stretching), not per transaction: the 181-byte IS31FL3731 flush (spec 01
    // §7.3) alone takes ~4.4 ms on the wire.
    Regs* r = regs();
    uint32_t t = micros(), polls = 0;
    while (!(r->S & S_IICIF)) {
        if (i2cWaitExpired(micros() - t, ++polls, board::kI2cTimeoutUs)) {
            if (r->S & S_IICIF) break;   // re-check: an ISR held the CPU, not the bus
            return kTimeout;
        }
    }
    r->S = S_IICIF;
    if (r->S & S_ARBL) { r->S = S_ARBL; return kArbLost; }
    return kOk;
}

I2cBus::Result I2cBus::start(uint8_t addrRw, bool repeated) {
    Regs* r = regs();
    if (repeated) {
        r->C1 = C1_IICEN | C1_MST | C1_TX | C1_RSTA;
    } else {
        uint32_t t = micros(), polls = 0;
        while (r->S & S_BUSY) {
            if (i2cWaitExpired(micros() - t, ++polls, board::kI2cTimeoutUs) && (r->S & S_BUSY)) {
                // spec 01 §7.5: bus busy > 10 ms -> recovery (when really stuck).
                uint32_t now = millis();
                if (!busySinceMs_) busySinceMs_ = now ? now : 1;
                else if (now - busySinceMs_ > board::kI2cBusyRecoverMs) { busySinceMs_ = 0; maybeRecover(); }
                return kTimeout;
            }
        }
        busySinceMs_ = 0;
        r->C1 = C1_IICEN | C1_MST | C1_TX;
    }
    r->D = addrRw;
    Result w = waitIf();
    if (w != kOk) return w;
    return (r->S & S_RXAK) ? kNak : kOk;
}

void I2cBus::stop() {
    Regs* r = regs();
    r->C1 = C1_IICEN;  // clearing MST generates STOP
    uint32_t t = micros();
    while ((r->S & S_BUSY) && micros() - t < 200) {}
}

I2cBus::Result I2cBus::txBytes(const uint8_t* data, uint32_t len) {
    Regs* r = regs();
    for (uint32_t i = 0; i < len; i++) {
        r->D = data[i];
        Result w = waitIf();
        if (w != kOk) return w;
        if (r->S & S_RXAK) return kNak;
    }
    return kOk;
}

I2cBus::Result I2cBus::rxBytes(uint8_t* data, uint32_t len) {
    Regs* r = regs();
    if (len == 0) return kOk;
    r->C1 = C1_IICEN | C1_MST | (len == 1 ? C1_TXAK : 0);
    (void)r->D;  // dummy read starts the first byte
    for (uint32_t i = 0; i < len; i++) {
        Result w = waitIf();
        if (w != kOk) return w;
        if (i == len - 1) {
            r->C1 = C1_IICEN | C1_MST | C1_TX;   // switch to TX before reading last byte: no extra clock
            data[i] = r->D;
        } else {
            if (i == len - 2) r->C1 = C1_IICEN | C1_MST | C1_TXAK;  // NACK the last byte
            data[i] = r->D;
        }
    }
    return kOk;
}

bool I2cBus::write(uint8_t addr, const uint8_t* data, uint32_t len) {
    Result e = start(uint8_t(addr << 1), false);
    if (e == kOk) e = txBytes(data, len);
    if (e != kOk) return fail(e);
    stop();
    return true;
}

bool I2cBus::read(uint8_t addr, uint8_t* data, uint32_t len) {
    Result e = start(uint8_t((addr << 1) | 1), false);
    if (e == kOk) e = rxBytes(data, len);
    if (e != kOk) return fail(e);
    stop();
    return true;
}

bool I2cBus::writeRead(uint8_t addr, const uint8_t* w, uint32_t wlen, uint8_t* rbuf, uint32_t rlen) {
    Result e = start(uint8_t(addr << 1), false);
    if (e == kOk) e = txBytes(w, wlen);
    if (e == kOk) e = start(uint8_t((addr << 1) | 1), true);
    if (e == kOk) e = rxBytes(rbuf, rlen);
    if (e != kOk) return fail(e);
    stop();
    return true;
}

bool I2cBus::probe(uint8_t addr) {
    // A NAK is the expected "absent" answer here and is not counted; bus faults are.
    Result e = start(uint8_t(addr << 1), false);
    if (e == kTimeout || e == kArbLost) return fail(e);
    stop();
    return e == kOk;
}

I2cBus& i2c0() { static I2cBus b(0); return b; }
I2cBus& i2c2() { static I2cBus b(2); return b; }
I2cBus& padBus() { return board::kPadBusIndex == 0 ? i2c0() : i2c2(); }

}  // namespace hal
}  // namespace ot
