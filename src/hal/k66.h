// OpenTracker -- small register helpers for the MK66 (NXP K66 reference manual).
// Registers missing from the Teensy kinetis.h (SDRAMC, FlexBus CSPMCR) live here.
#pragma once

#include <stdint.h>
#include "board.h"

namespace ot {
namespace hal {

inline volatile uint32_t& pcr(board::Pin p) {
    return *reinterpret_cast<volatile uint32_t*>(0x40049000u + p.port * 0x1000u + p.bit * 4u);
}
struct GpioRegs { volatile uint32_t PDOR, PSOR, PCOR, PTOR, PDIR, PDDR; };
inline GpioRegs& gpio(board::Pin p) {
    return *reinterpret_cast<GpioRegs*>(0x400FF000u + p.port * 0x40u);
}
// Drive an output: set level first, then direction (no glitch), then mux GPIO.
inline void gpioOutput(board::Pin p, bool high, uint32_t pcrVal = 0x144) {
    if (high) gpio(p).PSOR = 1u << p.bit; else gpio(p).PCOR = 1u << p.bit;
    gpio(p).PDDR |= 1u << p.bit;
    pcr(p) = pcrVal;
}
inline void gpioWrite(board::Pin p, bool high) {
    if (high) gpio(p).PSOR = 1u << p.bit; else gpio(p).PCOR = 1u << p.bit;
}
inline bool gpioRead(board::Pin p) { return (gpio(p).PDIR >> p.bit) & 1u; }

// SDRAM controller (K66 RM ch. "SDRAM Controller"), base 0x4000F000.
#define OT_SDRAM_CTRL (*(volatile uint16_t*)0x4000F042)
#define OT_SDRAM_AC0  (*(volatile uint32_t*)0x4000F048)
#define OT_SDRAM_CM0  (*(volatile uint32_t*)0x4000F04C)
constexpr uint32_t kSdramAcIp   = 0x00000008;  // precharge-all
constexpr uint32_t kSdramAcImrs = 0x00000040;  // mode-register set
constexpr uint32_t kSdramAcRe   = 0x00008000;  // auto-refresh enable
#define OT_FB_CSPMCR  (*(volatile uint32_t*)0x4000C060)

}  // namespace hal
}  // namespace ot
