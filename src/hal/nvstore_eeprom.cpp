// OpenTracker -- INvStore on the FlexRAM emulated EEPROM (spec 02 §9, spec 01 §10).
//
// The stock firmware's EEPROM accessors add 10 to every address, so the
// config image (spec 05 §5.4) starts at physical byte 10; bytes 0..9 (byte 0
// = update/power flag) are never touched here. Nothing is read or written
// unless the FlexRAM is already partitioned as EEPROM (fw_update.h
// eepromPartitionUsable()): otherwise the Teensy core would issue a PGMPART
// and repartition the data flash of a unit we have not confirmed.
#include "nvstore_eeprom.h"

#include <Arduino.h>
#include <avr/eeprom.h>

#include "fw_update.h"
#include "k66.h"

namespace ot {
namespace hal {
namespace {

constexpr uint32_t kAppBase = 10;      // spec 02 §9: accessors add 10
constexpr uint32_t kEepromSize = 4096; // spec 02 §9: 4 KiB FlexRAM window
volatile const uint8_t* const kFlexRam = reinterpret_cast<volatile const uint8_t*>(0x14000000);

class EepromNvStore final : public INvStore {
public:
    uint32_t size() const override { return kEepromSize - kAppBase; }

    bool read(uint32_t offset, void* dst, uint32_t len) override {
        if (!inRange(offset, len) || !ready()) return false;
        uint8_t* d = static_cast<uint8_t*>(dst);
        for (uint32_t i = 0; i < len; ++i) d[i] = kFlexRam[kAppBase + offset + i];
        return true;
    }

    bool write(uint32_t offset, const void* src, uint32_t len) override {
        if (!inRange(offset, len) || !ready()) return false;
        const uint8_t* s = static_cast<const uint8_t*>(src);
        for (uint32_t i = 0; i < len; ++i) {
            const uint32_t addr = kAppBase + offset + i;
            if (kFlexRam[addr] == s[i]) continue;   // only changed bytes cost an EEE write
            // eeprom_write_byte() leaves HSRUN around the FlexRAM write and
            // kinetis_hsrun_enable() then sets OUTDIV3 = 0 (FlexBus/SDRAM at
            // 180 MHz). Restore OUTDIV3 = 1 before anything can touch the
            // SDRAM (same sequence as fw_update.cpp, spec 01 §2 step 1).
            __disable_irq();
            eeprom_write_byte(reinterpret_cast<uint8_t*>(addr), s[i]);
            SIM_CLKDIV1 = (SIM_CLKDIV1 & ~SIM_CLKDIV1_OUTDIV3(15)) | SIM_CLKDIV1_OUTDIV3(1);
            __enable_irq();
            if (kFlexRam[addr] != s[i]) return false;
        }
        return true;
    }

private:
    bool inRange(uint32_t offset, uint32_t len) const { return offset <= size() && len <= size() - offset; }
    bool ready() {
        if (!checked_) {
            usable_ = eepromPartitionUsable();
            checked_ = true;
        }
        // Re-check the ready bit every time: the Teensy accessors would
        // re-run eeprom_initialize() (PGMPART) if it were clear.
        return usable_ && (FTFL_FCNFG & FTFL_FCNFG_EEERDY) && !(FTFL_FCNFG & FTFL_FCNFG_RAMRDY);
    }
    bool checked_ = false;
    bool usable_ = false;
};

EepromNvStore gNvStore;

}  // namespace

INvStore& nvStore() { return gNvStore; }

}  // namespace hal
}  // namespace ot
