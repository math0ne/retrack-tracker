// OpenTracker -- boot diagnostic log: RAM ring of timestamped text lines plus the
// fault/stuck report formatting. Platform independent (no registers, no printf)
// so it is host-tested (tests/diag_log_test.cpp) and safe to call from a fault
// handler. The device side (diag.cpp) adds locking, the USB CDC drain and the
// fault capture; see docs/HARDWARE_TESTING.md "Diagnostic log".
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ot {
namespace diag {

// Bounded text builder: never overruns, always NUL-terminated (cap >= 1).
struct Fmt {
    char* buf;
    size_t cap;
    size_t len = 0;
    Fmt(char* b, size_t c) : buf(b), cap(c) { if (cap) buf[0] = 0; }
    Fmt& str(const char* s);
    Fmt& ch(char c);
    Fmt& hex(uint32_t v, int digits = 8);   // "0x" + digits
    Fmt& dec(uint32_t v, int width = 0);    // right aligned, space padded
    Fmt& ms(uint32_t ms);                   // "[  12.345] "
};

constexpr uint32_t kRingSize = 8192;    // power of two (SRAM budget: 2026-10-15 field build had 25 KB stack headroom)
constexpr uint32_t kBootSize = 6144;    // first lines of the boot, kept for replay (field boot logs ~4 KB)

struct LogBuf {
    char ring[kRingSize];
    uint32_t head = 0;        // total bytes ever appended (ring index = head % size)
    uint32_t txPos = 0;       // ring stream position already handed to the drain
    uint32_t dropped = 0;     // bytes overwritten before they were sent
    char boot[kBootSize];
    uint32_t bootLen = 0;     // append-only copy of the first kBootSize bytes
    bool bootFrozen = false;  // stop copying into boot[] (bring-up finished)
    uint32_t replayPos = 0;   // replay cursor into boot[] (== replayEnd: idle)
    uint32_t replayEnd = 0;
    const char* extra = nullptr;   // one-off block sent before everything else
    uint32_t extraLen = 0, extraPos = 0;
};

void reset(LogBuf& b);
void append(LogBuf& b, const char* s, size_t n);
// "[   s.mmm] text\r\n"
void line(LogBuf& b, uint32_t ms, const char* text);
// Copies up to `max` pending bytes (extra block, then replay, then ring stream).
size_t take(LogBuf& b, char* out, size_t max);
bool pending(const LogBuf& b);
// Re-send the boot copy (host command: any received byte).
void requestReplay(LogBuf& b);
// Ring position (monotonic, like head) where the last `lines` lines start.
uint32_t tailStart(const LogBuf& b, unsigned lines);
// Copies the ring from `from` to head into out (NUL-terminated); returns length.
size_t copyFrom(const LogBuf& b, uint32_t from, char* out, size_t cap);

// Fault snapshot (ARMv7-M exception frame + SCB fault status).
struct FaultInfo {
    uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;
    uint32_t sp, excReturn, ipsr;
    uint32_t cfsr, hfsr, mmfar, bfar;
    uint32_t stage;
    const char* stageName;
    uint32_t ms;
    bool frameValid;
};
const char* exceptionName(uint32_t ipsr);
// Multi-line report ("!!! FAULT ..." ... "decoded: ..."), CRLF line ends.
size_t formatFault(char* out, size_t cap, const FaultInfo& f);
// Appends the decoded CFSR/HFSR bit names, e.g. "PRECISERR BFARVALID FORCED".
void decodeFault(Fmt& o, uint32_t cfsr, uint32_t hfsr);

}  // namespace diag
}  // namespace ot
