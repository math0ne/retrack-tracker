// OpenTracker -- periodic app telemetry line for the diag log (PLAN_PHASE2 T1c,
// docs/HARDWARE_TESTING.md "Diagnostic log"). Platform independent (no
// registers, no printf) so it is host-tested (tests/telemetry_test.cpp); the
// device glue (src/app_main.cpp) feeds it from the main loop and sends the
// line through diagLog() every 5 s while a USB terminal is open.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ot {
namespace diag {

// Audio render budget per 128-frame block: 128 / 44117.6 Hz * 180 MHz.
constexpr uint32_t kRenderBudgetCycles = 522240;
// Hardware rounds alarm at 70 % of the budget (docs/PLAN_PHASE2.md "Rules").
constexpr uint32_t kRenderAlarmPermille = 700;

struct TelemetrySample {
    uint32_t cycMin = 0, cycAvg = 0, cycMax = 0;   // DWT cycles per render block
    uint32_t blocks = 0;                           // render blocks in the window
    uint32_t loopMaxUs = 0;                        // longest main-loop iteration
    uint32_t loops = 0;                            // iterations in the window
    uint32_t sdramFree = 0;                        // bytes not used by the SDRAM layout
    uint32_t i2c0Errors = 0, i2c2Errors = 0;       // cumulative bus faults (timeouts, arbitration)
    uint32_t i2c0Naks = 0, i2c2Naks = 0;           // cumulative NAKs (shown when non-zero)
};

// Main-loop timing over a report window.
class Telemetry {
public:
    static constexpr uint32_t kPeriodMs = 5000;

    // Once per main-loop iteration (microsecond clock, wraps).
    void loopMark(uint32_t nowUs);
    // True once per kPeriodMs (the first call only starts the window).
    bool due(uint32_t nowMs);
    uint32_t loopMaxUs() const { return loopMaxUs_; }
    uint32_t loops() const { return loops_; }
    // Starts a new loop-timing window (after a report, or a skipped one).
    void resetWindow();

private:
    uint32_t lastUs_ = 0;
    bool haveLast_ = false;
    uint32_t loopMaxUs_ = 0;
    uint32_t loops_ = 0;
    uint32_t lastReportMs_ = 0;
    bool started_ = false;
};

// "telemetry: render cyc/block min A avg B max C (P.P% of 522240) blocks N |
//  loop max M.MMM ms (n L) | SDRAM free F B | i2c errors i2c0 X i2c2 Y[ nak i2c0 A i2c2 B]"
// with " RENDER>70%" appended when the max crosses the alarm. Returns length.
size_t formatTelemetry(char* out, size_t cap, const TelemetrySample& s);

}  // namespace diag
}  // namespace ot
