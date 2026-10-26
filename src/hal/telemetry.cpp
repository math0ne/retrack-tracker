// OpenTracker -- app telemetry line (telemetry.h). Platform independent.
#include "telemetry.h"

#include "diag_log.h"

namespace ot {
namespace diag {

void Telemetry::loopMark(uint32_t nowUs) {
    if (haveLast_) {
        const uint32_t d = nowUs - lastUs_;   // unsigned: wrap-safe
        if (d > loopMaxUs_) loopMaxUs_ = d;
        ++loops_;
    }
    lastUs_ = nowUs;
    haveLast_ = true;
}

bool Telemetry::due(uint32_t nowMs) {
    if (!started_) {
        started_ = true;
        lastReportMs_ = nowMs;
        return false;
    }
    if (nowMs - lastReportMs_ < kPeriodMs) return false;
    lastReportMs_ = nowMs;
    return true;
}

void Telemetry::resetWindow() {
    loopMaxUs_ = 0;
    loops_ = 0;
    // The iteration in progress (it includes the report itself) still counts:
    // keep lastUs_ so the next mark measures it.
}

size_t formatTelemetry(char* out, size_t cap, const TelemetrySample& s) {
    Fmt o(out, cap);
    const uint32_t permille = uint32_t(uint64_t(s.cycMax) * 1000u / kRenderBudgetCycles);
    o.str("telemetry: render cyc/block min ").dec(s.cycMin).str(" avg ").dec(s.cycAvg).str(" max ").dec(s.cycMax);
    o.str(" (").dec(permille / 10).ch('.').dec(permille % 10).str("% of ").dec(kRenderBudgetCycles);
    o.str(") blocks ").dec(s.blocks);
    o.str(" | loop max ").dec(s.loopMaxUs / 1000).ch('.');
    const uint32_t f = s.loopMaxUs % 1000;
    o.ch(char('0' + f / 100)).ch(char('0' + f / 10 % 10)).ch(char('0' + f % 10));
    o.str(" ms (n ").dec(s.loops).str(")");
    o.str(" | SDRAM free ").dec(s.sdramFree).str(" B");
    o.str(" | i2c errors i2c0 ").dec(s.i2c0Errors).str(" i2c2 ").dec(s.i2c2Errors);
    if (s.i2c0Naks || s.i2c2Naks) o.str(" nak i2c0 ").dec(s.i2c0Naks).str(" i2c2 ").dec(s.i2c2Naks);
    if (permille >= kRenderAlarmPermille) o.str(" RENDER>70%");
    return o.len;
}

}  // namespace diag
}  // namespace ot
