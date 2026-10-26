// OpenTracker -- boot diagnostic log (see diag_log.h). No libc formatting, so it
// is usable from the fault handler.
#include "diag_log.h"

namespace ot {
namespace diag {

static_assert((kRingSize & (kRingSize - 1)) == 0, "ring size must be a power of two");

Fmt& Fmt::ch(char c) {
    if (len + 1 < cap) {
        buf[len++] = c;
        buf[len] = 0;
    }
    return *this;
}
Fmt& Fmt::str(const char* s) {
    if (!s) s = "(null)";
    while (*s) ch(*s++);
    return *this;
}
Fmt& Fmt::hex(uint32_t v, int digits) {
    str("0x");
    for (int i = digits - 1; i >= 0; i--) ch("0123456789ABCDEF"[(v >> (4 * i)) & 0xF]);
    return *this;
}
Fmt& Fmt::dec(uint32_t v, int width) {
    char t[10];
    int n = 0;
    do { t[n++] = char('0' + v % 10); v /= 10; } while (v);
    for (int i = n; i < width; i++) ch(' ');
    while (n) ch(t[--n]);
    return *this;
}
Fmt& Fmt::ms(uint32_t m) {
    ch('[');
    dec(m / 1000, 5);
    ch('.');
    uint32_t f = m % 1000;
    ch(char('0' + f / 100)).ch(char('0' + f / 10 % 10)).ch(char('0' + f % 10));
    return str("] ");
}

void reset(LogBuf& b) {
    b.head = b.txPos = b.dropped = 0;
    b.bootLen = b.replayPos = b.replayEnd = 0;
    b.bootFrozen = false;
    b.extra = nullptr;
    b.extraLen = b.extraPos = 0;
}

void append(LogBuf& b, const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        b.ring[b.head & (kRingSize - 1)] = s[i];
        b.head++;
        if (!b.bootFrozen && b.bootLen < kBootSize) b.boot[b.bootLen++] = s[i];
    }
    if (b.head - b.txPos > kRingSize) {   // the drain fell behind: oldest bytes are gone
        b.dropped += b.head - b.txPos - kRingSize;
        b.txPos = b.head - kRingSize;
    }
}

void line(LogBuf& b, uint32_t ms, const char* text) {
    char pre[24];
    Fmt f(pre, sizeof pre);
    f.ms(ms);
    append(b, pre, f.len);
    size_t n = 0;
    while (text[n]) n++;
    append(b, text, n);
    append(b, "\r\n", 2);
}

bool pending(const LogBuf& b) {
    return (b.extra && b.extraPos < b.extraLen) || b.replayPos < b.replayEnd || b.txPos != b.head;
}

size_t take(LogBuf& b, char* out, size_t max) {
    size_t n = 0;
    while (n < max && b.extra && b.extraPos < b.extraLen) out[n++] = b.extra[b.extraPos++];
    while (n < max && b.replayPos < b.replayEnd) out[n++] = b.boot[b.replayPos++];
    while (n < max && b.txPos != b.head) out[n++] = b.ring[b.txPos++ & (kRingSize - 1)];
    return n;
}

void requestReplay(LogBuf& b) {
    b.replayPos = 0;
    b.replayEnd = b.bootLen;
}

uint32_t tailStart(const LogBuf& b, unsigned lines) {
    uint32_t oldest = b.head > kRingSize ? b.head - kRingSize : 0;
    uint32_t p = b.head;
    // Skip the final line terminator, then count newlines backwards.
    if (p > oldest && b.ring[(p - 1) & (kRingSize - 1)] == '\n') p--;
    unsigned seen = 0;
    while (p > oldest) {
        if (b.ring[(p - 1) & (kRingSize - 1)] == '\n' && ++seen == lines) return p;
        p--;
    }
    return oldest;
}

size_t copyFrom(const LogBuf& b, uint32_t from, char* out, size_t cap) {
    if (!cap) return 0;
    uint32_t oldest = b.head > kRingSize ? b.head - kRingSize : 0;
    if (from < oldest) from = oldest;
    size_t n = 0;
    while (from != b.head && n + 1 < cap) out[n++] = b.ring[from++ & (kRingSize - 1)];
    out[n] = 0;
    return n;
}

const char* exceptionName(uint32_t ipsr) {
    switch (ipsr & 0x1FF) {
        case 2: return "NMI";
        case 3: return "HardFault";
        case 4: return "MemManage";
        case 5: return "BusFault";
        case 6: return "UsageFault";
        default: return "exception";
    }
}

void decodeFault(Fmt& o, uint32_t cfsr, uint32_t hfsr) {
    static const struct { uint32_t bit; const char* name; } kBits[] = {
        {1u << 0, "IACCVIOL"}, {1u << 1, "DACCVIOL"}, {1u << 3, "MUNSTKERR"}, {1u << 4, "MSTKERR"},
        {1u << 5, "MLSPERR"}, {1u << 7, "MMARVALID"},
        {1u << 8, "IBUSERR"}, {1u << 9, "PRECISERR"}, {1u << 10, "IMPRECISERR"}, {1u << 11, "UNSTKERR"},
        {1u << 12, "STKERR"}, {1u << 13, "LSPERR"}, {1u << 15, "BFARVALID"},
        {1u << 16, "UNDEFINSTR"}, {1u << 17, "INVSTATE"}, {1u << 18, "INVPC"}, {1u << 19, "NOCP"},
        {1u << 24, "UNALIGNED"}, {1u << 25, "DIVBYZERO"},
    };
    bool any = false;
    for (const auto& k : kBits)
        if (cfsr & k.bit) { o.ch(' ').str(k.name); any = true; }
    if (hfsr & (1u << 1)) { o.str(" VECTTBL"); any = true; }
    if (hfsr & (1u << 30)) { o.str(" FORCED"); any = true; }
    if (hfsr & (1u << 31)) { o.str(" DEBUGEVT"); any = true; }
    if (!any) o.str(" (none)");
}

size_t formatFault(char* out, size_t cap, const FaultInfo& f) {
    Fmt o(out, cap);
    o.str("!!! FAULT ").str(exceptionName(f.ipsr)).str(" (IPSR ").dec(f.ipsr & 0x1FF)
        .str(") at boot stage ").dec(f.stage).str(" '").str(f.stageName ? f.stageName : "?")
        .str("' t=").dec(f.ms).str(" ms\r\n");
    if (f.frameValid) {
        o.str("PC ").hex(f.pc).str("  LR ").hex(f.lr).str("  xPSR ").hex(f.xpsr).str("\r\n");
        o.str("R0 ").hex(f.r0).str("  R1 ").hex(f.r1).str("  R2 ").hex(f.r2).str("  R3 ").hex(f.r3)
            .str("  R12 ").hex(f.r12).str("\r\n");
    } else {
        o.str("exception frame not readable (stack pointer outside SRAM)\r\n");
    }
    o.str("SP ").hex(f.sp).str((f.excReturn & 4) ? " (PSP)" : " (MSP)").str("  EXC_RETURN ").hex(f.excReturn)
        .str("\r\n");
    o.str("CFSR ").hex(f.cfsr).str("  HFSR ").hex(f.hfsr).str("  MMFAR ").hex(f.mmfar).str("  BFAR ")
        .hex(f.bfar).str("\r\n");
    o.str("decoded:");
    decodeFault(o, f.cfsr, f.hfsr);
    o.str("\r\n");
    return o.len;
}

}  // namespace diag
}  // namespace ot
