// OpenTracker -- hand-off to the board's update bootloader (spec 02 §9, spec 01 §10).
#include "fw_update.h"

#include <Arduino.h>
#include <avr/eeprom.h>
#include <string.h>

#include "board.h"
#include "k66.h"

namespace ot {
namespace hal {
namespace {

constexpr const char* kFwDir = "/Firmware";
constexpr const char* kFwInfo = "/Firmware/_fwinfo";
constexpr const char* kTrigger = "/Firmware/ot_restore.txt";
constexpr const char* kTriggerDone = "/Firmware/ot_restore.done";
constexpr uint32_t kMaxName = 96;

bool startsWith(const char* s, const char* prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

bool endsWithNoCase(const char* s, const char* suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    if (n < m) return false;
    return strcasecmp(s + n - m, suffix) == 0;
}

bool imagePath(const char* name, char* out, size_t outLen) {
    int n = snprintf(out, outLen, "%s/%s", kFwDir, name);
    return n > 0 && static_cast<size_t>(n) < outLen;
}

}  // namespace

FlexRamInfo readFlexRamInfo() {
    FlexRamInfo f = {};
    f.fcfg1 = SIM_FCFG1;
    f.fcnfg = FTFL_FCNFG;
    f.eesize = uint8_t((f.fcfg1 >> 16) & 0xF);
    f.depart = uint8_t((f.fcfg1 >> 8) & 0xF);
    const volatile uint8_t* flexRam = reinterpret_cast<const volatile uint8_t*>(0x14000000);
    // FlexRAM is only accessible once it is ready as RAM or as EEPROM.
    if (f.fcnfg & (FTFL_FCNFG_RAMRDY | FTFL_FCNFG_EEERDY))
        for (int i = 0; i < 16; i++) f.head[i] = flexRam[i];
    return f;
}

bool eepromPartitionUsable() {
    // EEERDY can take a moment after reset while the EEE is restored from flash.
    for (uint32_t t = millis(); millis() - t < 100;) {
        uint8_t c = FTFL_FCNFG;
        if (c & FTFL_FCNFG_RAMRDY) return false;   // plain RAM: never partitioned as EEPROM
        if (c & FTFL_FCNFG_EEERDY) return true;
    }
    return false;
}

bool isUpdateImageName(const char* name) {
    if (!name || strlen(name) < 15 || strlen(name) > kMaxName) return false;
    if (strchr(name, '/') || strchr(name, '\\')) return false;
    return (startsWith(name, "PolyendTrac") && endsWithNoCase(name, ".ptf")) ||
           (startsWith(name, "mt") && endsWithNoCase(name, ".hex"));
}

bool requestFirmwareUpdate(IStorage& fs, const char* imageName) {
    char path[128];
    if (!isUpdateImageName(imageName) || !imagePath(imageName, path, sizeof path)) return false;
    if (!fs.exists(path)) return false;
    // Refuse before touching anything if EEPROM byte 0 cannot be written without
    // the Teensy core repartitioning the FlexRAM (eeprom_initialize() PGMPART).
    if (!eepromPartitionUsable()) return false;

    // spec 02 §9 steps 1-2: replace _fwinfo with the raw name (no terminator, no header).
    if (fs.exists(kFwInfo) && !fs.remove(kFwInfo)) return false;
    IStorage::Handle h = fs.open(kFwInfo, IStorage::kWrite);
    if (h < 0) return false;
    const uint32_t len = static_cast<uint32_t>(strlen(imageName));
    const bool written = fs.write(h, imageName, len) == static_cast<int32_t>(len);
    fs.close(h);
    if (!written) return false;

    // spec 02 §9 step 3 / spec 01 §10: EEPROM byte 0 = 1, then 10 ms, then PTA1 low.
    // EEPROM byte 0 is FlexRAM byte 0 (spec 01 §10 "physical byte 0 = power flag").
    // The Teensy core only issues PGMPART when the FlexRAM has never been set up as
    // EEPROM (FCNFG.RAMRDY); on a unit that ran stock firmware the stock partition
    // is kept, so byte 0 means the same thing to both firmwares.
    // The Teensy core's eeprom_write_byte() leaves HSRUN around the FlexRAM write
    // (K66: no flash programming in HSRUN) and kinetis_hsrun_enable() then rewrites
    // SIM_CLKDIV1 with OUTDIV3 = 0, i.e. FlexBus/SDRAM at 180 MHz. Put OUTDIV3 = 1
    // (90 MHz, spec 01 §2 step 1) back before anything can touch the SDRAM again.
    __disable_irq();
    eeprom_write_byte(reinterpret_cast<uint8_t*>(0), 1);
    SIM_CLKDIV1 = (SIM_CLKDIV1 & ~SIM_CLKDIV1_OUTDIV3(15)) | SIM_CLKDIV1_OUTDIV3(1);
    __enable_irq();
    delay(10);
    gpioOutput(board::kPinUpdateReq, false);
    for (;;) {
    }
}

void checkRestoreTrigger(IStorage& fs) {
    if (!fs.exists(kTrigger)) return;
    char name[kMaxName + 1] = {};
    IStorage::Handle h = fs.open(kTrigger, IStorage::kRead);
    if (h < 0) return;
    int32_t n = fs.read(h, name, kMaxName);
    fs.close(h);
    if (n <= 0) return;
    name[n] = '\0';
    // First line only, trimmed.
    for (char* p = name; *p; ++p) {
        if (*p == '\r' || *p == '\n') { *p = '\0'; break; }
    }
    size_t len = strlen(name);
    while (len && (name[len - 1] == ' ' || name[len - 1] == '\t')) name[--len] = '\0';

    // Consume the trigger first so a bootloader that rejects the image cannot
    // send us into a reset loop.
    fs.remove(kTriggerDone);
    if (!fs.rename(kTrigger, kTriggerDone)) return;
    requestFirmwareUpdate(fs, name);
}

}  // namespace hal
}  // namespace ot
