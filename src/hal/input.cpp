// OpenTracker -- keypads + encoder (spec 01 §7.1/§7.2/§7.4, spec 05 §2.1).
// TCA8418 register map from the TI datasheet; FTM quadrature mode from the K66 RM.
#include "input.h"

#include <Arduino.h>
#include "diag.h"
#include "i2c_bus.h"
#include "k66.h"

namespace ot {
namespace hal {

namespace {
// TCA8418 registers (TI datasheet).
constexpr uint8_t REG_CFG = 0x01, REG_INT_STAT = 0x02, REG_KEY_LCK_EC = 0x03,
    REG_KEY_EVENT_A = 0x04, REG_KP_GPIO1 = 0x1D, REG_KP_GPIO2 = 0x1E, REG_KP_GPIO3 = 0x1F;

// spec 05 §2.1.1 key map: TCA code (row*10 + col + 1) -> button id, 0xFF = unused.
uint8_t buttonFromCode(uint8_t code) {
    if (code == 0 || code > 80) return 0xFF;
    uint8_t row = uint8_t((code - 1) / 10), col = uint8_t((code - 1) % 10);
    if (row <= 4 && col <= 4) return uint8_t(8 + row * 5 + col);
    if (row == 7 && col <= 7) return uint8_t(7 - col);
    return 0xFF;
}

// spec 05 §2.1.2: codes 1..10 -> pads 0..9, 11..20 -> 12..21, ...; extra columns
// (codes 51/52, 61/62, 71/72, 81/82) -> pads 11/10, 23/22, 35/34, 47/46.
// UNVERIFIED: spec 01 §7.2 describes the extra keys as "cols 8-9 of rows 0-7";
// spec 05 gives the explicit table used here. The selftest shows raw codes.
uint8_t padFromCode(uint8_t code) {
    if (code == 0 || code > 80) return 0xFF;
    uint8_t row = uint8_t((code - 1) / 10), col = uint8_t((code - 1) % 10);
    if (row < 4) return uint8_t(row * board::kPadColumns + col);
    // Pad columns 10/11 sit on keypad rows 4-7, columns 9/8 (mapped on the real
    // device 2026-10-15: pad row r col 10 = code (r+4)*10+10, col 11 = (r+4)*10+9).
    if (col == 9) return uint8_t((row - 4) * board::kPadColumns + 10);
    if (col == 8) return uint8_t((row - 4) * board::kPadColumns + 11);
    return 0xFF;
}

// spec 01 §7.1 / spec 05 §2.1.1 hold timing: T = 333 ms, accel table.
constexpr uint32_t kHoldMs = 333;
constexpr uint16_t kHoldAccel[6] = {0, 100, 190, 235, 255, 270};
// spec 05 §2.1.1: auto-repeat ids 0x19..0x1b and 0x1e..0x21 (0x21 is not on this matrix).
bool autoRepeats(uint8_t id) { return (id >= 0x19 && id <= 0x1B) || (id >= 0x1E && id <= 0x21); }
}  // namespace

bool PanelInput::initChip(I2cBus& bus, uint8_t gpio3) {
    // spec 01 §7.1 init sequence
    bool ok = bus.writeReg8(board::kAddrButtons, REG_KP_GPIO1, 0xFF);
    ok = bus.writeReg8(board::kAddrButtons, REG_KP_GPIO2, 0xFF) && ok;
    ok = bus.writeReg8(board::kAddrButtons, REG_KP_GPIO3, gpio3) && ok;
    ok = bus.writeReg8(board::kAddrButtons, REG_CFG, board::kTcaCfg) && ok;
    // Drain any stale events, then clear the interrupt status.
    for (int i = 0; i < 10 && ok; i++) {
        uint8_t ec = 0;
        if (!bus.readReg8(board::kAddrButtons, REG_KEY_LCK_EC, ec) || (ec & 0x0F) == 0) break;
        uint8_t e;
        bus.readReg8(board::kAddrButtons, REG_KEY_EVENT_A, e);
    }
    ok = bus.writeReg8(board::kAddrButtons, REG_INT_STAT, 0x0F) && ok;
    return ok;
}

bool PanelInput::init() {
    static_assert(board::kAddrButtons == board::kAddrPads, "same TCA8418 address on both buses");
    pinMode(board::kTeensyButtonsInt, INPUT_PULLUP);   // spec 01 §7.1
    pinMode(board::kTeensyPadsInt, INPUT_PULLUP);      // spec 01 §7.2
    buttonsOk_ = initChip(i2c0(), board::kTcaGpio3Buttons);
    padsOk_ = initChip(padBus(), board::kTcaGpio3Pads);

    // Encoder: FTM2 hardware quadrature decoder, exactly as stock (spec 09 §2, write
    // order taken from disassembly). SC stays 0 (no clock) for the life of the
    // firmware; the decoder counts without it. No FTM2 interrupt is installed.
    SIM_SCGC3 |= SIM_SCGC3_FTM2;
    pcr(board::kEncA) = board::kEncPcr;     // 0x612
    pcr(board::kEncB) = board::kEncPcr;
    // spec 09 §2, first routine
    FTM2_MODE = 0x05;
    FTM2_C0SC = 0;
    FTM2_C1SC = 0;
    FTM2_CNT = 0;
    FTM2_MOD = 0;
    FTM2_SC = 0;
    FTM2_FILTER = 0x44;
    FTM2_CNTIN = 0;
    FTM2_MOD = 0xFFFF;
    FTM2_CNT = 0;
    FTM2_C0SC = 0x10;
    FTM2_COMBINE = 0;
    FTM2_C0V = 0x6000;
    FTM2_FMS = 0x40;
    FTM2_QDCTRL = 0xC1;
    // spec 09 §2, second routine (tail call)
    (void)FTM2_FMS;
    FTM2_MODE = 0x05;
    FTM2_QDCTRL = 0xC0;
    FTM2_CNT = 0;
    (void)FTM2_SC;
    FTM2_SC = 0;
    (void)FTM2_C0SC;
    FTM2_C0SC = 0x10;
    FTM2_C0V = (FTM2_QDCTRL & 0x04) ? 0x6000 : 0xA000;   // QUADIR
    FTM2_QDCTRL = 0xC1;
    FTM2_FMS = 0x40;
    FTM2_C0SC = 0x50;
    diagLogf("enc init: SC 0x%02lX MOD 0x%04lX CNTIN 0x%04lX MODE 0x%02lX QD 0x%02lX",
             static_cast<unsigned long>(FTM2_SC), static_cast<unsigned long>(FTM2_MOD),
             static_cast<unsigned long>(FTM2_CNTIN), static_cast<unsigned long>(FTM2_MODE),
             static_cast<unsigned long>(FTM2_QDCTRL));
    encLast_ = millis();
    return buttonsOk_ && padsOk_;
}

void PanelInput::push(InputEvent::Type t, uint8_t id, int8_t delta) {
    uint32_t next = (qHead_ + 1) % kQueue;
    if (next == qTail_) return;   // full: drop newest
    InputEvent& e = q_[qHead_];
    e.type = t;
    e.id = id;
    e.velocity = (t == InputEvent::kPadDown) ? 127 : 0;   // spec 05 §2.1.2: no pressure
    e.delta = delta;
    e.timeMs = millis();
    qHead_ = next;
}

void PanelInput::readChip(I2cBus& bus, bool pads) {
    // spec 01 §7.1 service
    uint8_t ec = 0;
    if (!bus.readReg8(board::kAddrButtons, REG_KEY_LCK_EC, ec)) return;
    uint8_t n = ec & 0x0F;
    for (uint8_t i = 0; i < n; i++) {
        uint8_t ev = 0;
        if (!bus.readReg8(board::kAddrButtons, REG_KEY_EVENT_A, ev)) break;
        bool press = ev & 0x80;
        uint8_t code = ev & 0x7F;
        // Field mapping aid: every raw keypad event, with the id we map it to.
        diagLogf("key %s %s code %u (row %u col %u) -> id %d", pads ? "PAD" : "BTN",
                 press ? "down" : "up  ", unsigned(code), unsigned((code - 1) / 10),
                 unsigned((code - 1) % 10),
                 int(pads ? padFromCode(code) : buttonFromCode(code)) == 0xFF
                     ? -1
                     : int(pads ? padFromCode(code) : buttonFromCode(code)));
        if (pads) {
            lastPadCode_ = ev;
            uint8_t id = padFromCode(code);
            if (id == 0xFF) continue;
            if (press) pads_ |= 1ull << id; else pads_ &= ~(1ull << id);
            push(press ? InputEvent::kPadDown : InputEvent::kPadUp, id);
        } else {
            lastButtonCode_ = ev;
            uint8_t id = buttonFromCode(code);
            if (id == 0xFF) continue;
            if (press) {
                buttons_ |= 1ull << id;
                holdLevel_[id] = 0;
                holdNext_[id] = millis() + kHoldMs;
            } else {
                buttons_ &= ~(1ull << id);
            }
            push(press ? InputEvent::kButtonDown : InputEvent::kButtonUp, id);
        }
    }
    bus.writeReg8(board::kAddrButtons, REG_INT_STAT, 0x0F);
}

void PanelInput::serviceHolds(uint32_t now) {
    // IInput has no "hold" event type; spec 05 §2.1.1 re-sends holds of the
    // navigation keys as presses, which is what we emit (auto-repeat).
    for (uint8_t id = 0; id < board::kButtonCount; id++) {
        if (!((buttons_ >> id) & 1) || !autoRepeats(id)) continue;
        if (int32_t(now - holdNext_[id]) < 0) continue;
        push(InputEvent::kButtonDown, id);
        if (holdLevel_[id] < 5) holdLevel_[id]++;
        holdNext_[id] = now + (kHoldMs - kHoldAccel[holdLevel_[id]]);
    }
}

void PanelInput::serviceEncoder(uint32_t now) {
    // spec 09 §4 poll algorithm.
    // 2. one decode per >=101 ms
    if (now - encLast_ <= 100) return;
    encLast_ = now;
    // 3. read SC (bail on TOIE, never set), read CNT, CNT <- 0, discard the overflow band
    if (FTM2_SC & 0x40) return;
    const uint16_t u = uint16_t(FTM2_CNT);
    FTM2_CNT = 0;
    if (u) diagLogf("enc raw cnt 0x%04X", unsigned(u));
    if (u >= 0x4000 && u <= 0xC000) return;
    const int16_t v = int16_t(u);
    if (v == 0) return;
    const uint8_t dir = v > 0 ? 1 : 2;
    // 4. threshold = 96 / resolution(24) = 4 counts
    constexpr int16_t thr = board::kEncCountsPerDetent;
    // 5. remainder only carries over while the direction is unchanged
    const int16_t sum = int16_t((dir == encLastDir_ ? encRemainder_ : 0) + v);
    encLastDir_ = dir;
    if (sum > -thr && sum < thr) {
        encRemainder_ = sum;
        return;
    }
    // 6. truncating division, remainder keeps the sign of sum
    const int16_t q = int16_t(sum / thr);
    encRemainder_ = int16_t(sum - q * thr);
    // 7. acceleration, mode 2 (spec 09 §4 table row 2)
    static constexpr int16_t kAccel[10] = {0, 1, 2, 6, 12, 20, 30, 45, 60, 80};
    const int16_t mag = q > 0 ? q : int16_t(-q);
    const int16_t idx = mag > 49 ? 49 : mag;
    const int16_t acc = idx < 10 ? kAccel[idx] : 100;
    const int16_t r = q > 0 ? acc : int16_t(-acc);
    encTotal_ += r;
    diagLogf("enc steps %d -> %d", int(q), int(r));
    push(InputEvent::kEncoder, 0, int8_t(r));
}

void PanelInput::watchdog(uint32_t now) {
    // spec 01 §7.1 chip-reset watchdog: CFG read back; differs -> release + re-init.
    if (now - wdLast_ < 1000) return;
    wdLast_ = now;
    uint8_t cfg = 0;
    if (!i2c0().readReg8(board::kAddrButtons, REG_CFG, cfg) || cfg != board::kTcaCfg) {
        if (i2c0().errorCount()) i2c0().recover();
        for (uint8_t id = 0; id < board::kButtonCount; id++)
            if ((buttons_ >> id) & 1) push(InputEvent::kButtonUp, id);
        buttons_ = 0;
        buttonsOk_ = initChip(i2c0(), board::kTcaGpio3Buttons);
    }
    if (!padBus().readReg8(board::kAddrPads, REG_CFG, cfg) || cfg != board::kTcaCfg) {
        if (padBus().errorCount()) padBus().recover();
        for (uint8_t id = 0; id < board::kPadCount; id++)
            if ((pads_ >> id) & 1) push(InputEvent::kPadUp, id);
        pads_ = 0;
        padsOk_ = initChip(padBus(), board::kTcaGpio3Pads);
    }
}

void PanelInput::service() {
    if (inService_) return;
    inService_ = true;
    uint32_t now = millis();
    if (digitalReadFast(board::kTeensyButtonsInt) == LOW) readChip(i2c0(), false);
    if (digitalReadFast(board::kTeensyPadsInt) == LOW) readChip(padBus(), true);
    serviceHolds(now);
    serviceEncoder(now);
    watchdog(now);
    inService_ = false;
}

bool PanelInput::poll(InputEvent& ev) {
    service();
    if (qTail_ == qHead_) return false;
    ev = q_[qTail_];
    qTail_ = (qTail_ + 1) % kQueue;
    return true;
}

}  // namespace hal
}  // namespace ot
