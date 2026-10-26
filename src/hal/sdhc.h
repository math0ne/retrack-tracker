// OpenTracker -- SD card on the K66 SDHC, 4-bit bus (spec 01 §6). ADMA2 multi-block
// transfers with CMD6 high speed (like stock); polled PIO kept as runtime fallback.
#pragma once

#include <stdint.h>

namespace ot {
namespace hal {
namespace sdhc {

struct Info {
    bool adma = false;         // ADMA2 in use (false: polled PIO through DATPORT)
    bool highSpeed = false;    // CMD6 switch to high-speed timing accepted by the card
    uint32_t clockHz = 0;      // current transfer SDCLK
    uint32_t retries = 0;      // transfers retried after an error
    uint32_t degrades = 0;     // clock/mode fallbacks after repeated errors
    uint32_t bounced = 0;      // chunks copied through the SRAM bounce buffer
    uint32_t admaErr = 0;      // ADMAES of the last ADMA error
};

bool cardPresent();                 // spec 01 §6: PTD0 LOW = inserted
bool init();                        // identify + 4-bit + transfer clock; true when ready
bool ready();
void release();                     // stop using the card (unmount / removal / storage mode)
uint32_t sectorCount();             // 512-byte sectors, 0 if unknown
bool readBlocks(uint32_t lba, uint8_t* dst, uint32_t count);
bool writeBlocks(uint32_t lba, const uint8_t* src, uint32_t count);
uint32_t lastError();               // IRQSTAT bits of the last failure (diagnostics)
const Info& info();
const char* initNote();             // why ADMA/high speed were not used ("" when both are)

// ---- pure helpers (host-tested in tests/hw_math_test.cpp) --------------------
namespace detail {
// K66 RM SDHC "ADMA2 descriptor": attr bits Valid(0) End(1) Int(2) Act(5:4),
// Act = 10b "transfer data"; length in bits 31:16 (16 bits); then 32-bit address.
constexpr uint32_t kAdmaValid = 1u << 0, kAdmaEnd = 1u << 1, kAdmaTran = 2u << 4;
constexpr uint32_t adma2Attr(uint32_t len, bool last) {
    return (len << 16) | kAdmaValid | kAdmaTran | (last ? kAdmaEnd : 0u);
}
// Number of descriptors for `bytes` with at most `perDesc` bytes each.
constexpr uint32_t adma2DescCount(uint32_t bytes, uint32_t perDesc) {
    return (bytes + perDesc - 1u) / perDesc;
}

// K66 memory map: SRAM_L 0x1FFF0000..0x1FFFFFFF, SRAM_U 0x20000000..0x2002FFFF.
constexpr uint32_t kSramLBase = 0x1FFF0000u, kSramUBase = 0x20000000u, kSramUEnd = 0x20030000u;
// ADMA2 may target `addr..addr+len` directly: word aligned (K66 ADMA2 address
// alignment), inside one SRAM half (a burst must not cross the SRAM_L/SRAM_U
// boundary), or inside SDRAM when that path is allowed.
constexpr bool dmaDirectOk(uint32_t addr, uint32_t len, bool allowSdram, uint32_t sdramBase,
                           uint32_t sdramSize) {
    return (addr & 3u) == 0 && len > 0 &&
           ((addr >= kSramLBase && addr + len <= kSramUBase) ||
            (addr >= kSramUBase && addr + len <= kSramUEnd) ||
            (allowSdram && addr >= sdramBase && addr - sdramBase + len <= sdramSize));
}
// Core accesses to SRAM_L go through the LMEM code-bus cache; DMA does not.
constexpr bool inCodeBusRam(uint32_t addr) { return addr >= kSramLBase && addr < kSramUBase; }

// SD Physical Layer spec §4.3.10, CMD6 64-byte status (big-endian bit 511 first):
// bits 415:400 = group 1 support (bit 401 = high speed), bits 379:376 = group 1
// function selected (0xF = cannot switch).
inline bool cmd6HighSpeedSupported(const uint8_t* st) { return (st[13] & 0x02u) != 0; }
inline uint8_t cmd6Group1Result(const uint8_t* st) { return st[16] & 0x0Fu; }
}  // namespace detail

}  // namespace sdhc
}  // namespace hal
}  // namespace ot
