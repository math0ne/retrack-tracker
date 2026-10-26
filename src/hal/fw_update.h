// OpenTracker -- hand-off to the board's update bootloader (spec 02 §9, spec 01 §10).
//
// The flashing itself is done outside the MCU application: the application
// names the image in /Firmware/_fwinfo, sets EEPROM byte 0 = 1 and pulls the
// update-request line (PTA1) low. This is the only code allowed to drive PTA1.
#pragma once

#include <stdint.h>
#include "hal_api.h"

namespace ot {
namespace hal {

// Same filter the stock updater applies to the Firmware/ listing (spec 06,
// Config > Firmware update): length >= 15 and either "PolyendTrac*.ptf" or
// "mt*.hex". Only names the bootloader is known to accept are offered.
bool isUpdateImageName(const char* name);

// Write /Firmware/_fwinfo for `imageName` (a file inside /Firmware), set the
// update flag and request the update. Returns false (without touching PTA1)
// if the image is missing, the name is not acceptable or _fwinfo could not be
// written; otherwise it does not return.
bool requestFirmwareUpdate(IStorage& fs, const char* imageName);

// Read-only FlexRAM / EEPROM-emulation diagnostics (K66 RM "FTFE": SIM_FCFG1
// EESIZE bits 19:16 and DEPART bits 11:8, FTFE_FCNFG RAMRDY bit 1 / EEERDY bit 0;
// the EEPROM window is FlexRAM at 0x14000000). Never writes anything.
struct FlexRamInfo {
    uint32_t fcfg1;
    uint8_t fcnfg;
    uint8_t eesize;      // SIM_FCFG1.EESIZE code
    uint8_t depart;      // SIM_FCFG1.DEPART code
    uint8_t head[16];    // first 16 bytes of the EEPROM window (byte 0 = power flag)
};
FlexRamInfo readFlexRamInfo();
// True when the FlexRAM is partitioned and running as EEPROM (EEERDY, !RAMRDY).
// requestFirmwareUpdate() refuses otherwise, so the Teensy core can never issue a
// PGMPART (repartition) on a unit whose stock partition we have not confirmed.
bool eepromPartitionUsable();

// Boot-time escape hatch that needs only the SD card, not the display or
// buttons: if /Firmware/ot_restore.txt exists, its first line names an image
// in /Firmware (e.g. "PolyendTracker_1.9.2.ptf"). The trigger is renamed to
// ot_restore.done before the hand-off so a failed flash cannot loop.
// Returns only if there is no trigger or it is invalid.
void checkRestoreTrigger(IStorage& fs);

}  // namespace hal
}  // namespace ot
