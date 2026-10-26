// Device glue: hands the OpenTracker HAL drivers and the SDRAM to the looper.
// main.cpp (unchanged from OpenTracker) brings the board up, keeps the
// ot_restore.txt escape hatch, and calls otAppMain once the SDRAM tested OK;
// if this returns, the hardware selftest runs instead.
#include "hal/audio_sgtl5000.h"
#include "hal/board.h"
#include "hal/ft81x.h"
#include "hal/hal.h"
#include "hal/telemetry.h"
#include "looper/app.h"

namespace {

// SDRAM layout: loop buffers, then the waveform staging buffer at the top.
constexpr uint32_t kScratchBytes = 64 * 1024;
constexpr uint32_t kLoopBytes = ot::board::kSdramSize - kScratchBytes;
static_assert(looper::App::kWaveBytes <= kScratchBytes, "waveform staging buffer");

looper::App gApp;

bool selectInput(uint8_t source, uint8_t gain) { return ot::hal::selectInput(nullptr, source, gain); }

bool cpuLoad(uint32_t& avg, uint32_t& max) {
    uint32_t mn = 0, a = 0, mx = 0;
    if (!ot::hal::audio().renderCyclesWindow(mn, a, mx)) return false;
    avg = uint32_t(uint64_t(a) * 1000u / ot::diag::kRenderBudgetCycles);
    max = uint32_t(uint64_t(mx) * 1000u / ot::diag::kRenderBudgetCycles);
    return true;
}

uint32_t dlBytes() { return ot::hal::display().lastDlBytes(); }

void powerOff() { ot::hal::powerOff(); }

}  // namespace

void otAppMain(ot::hal::HalSet& hal) {
    using namespace ot;
    if (!hal::bootReport().sdramOk) return;
    // The codec init leaves the headphone amp muted (OpenTracker app_main.cpp).
    hal::audio().setHeadphoneVolume(0.7f);

    uint8_t* sd = reinterpret_cast<uint8_t*>(board::kSdramBase);
    looper::Platform p;
    p.display = hal.display;
    p.audio = hal.audio;
    p.input = hal.input;
    p.leds = hal.leds;
    p.clock = hal.clock;
    p.service = hal.service;
    p.lineIn = &hal::lineIn;
    p.selectInput = &selectInput;
    p.powerOff = &powerOff;
    p.cpuLoad = &cpuLoad;
    p.dlBytes = &dlBytes;
    p.loopMem = sd;
    p.loopMemBytes = kLoopBytes;
    p.scratch = sd + kLoopBytes;
    p.scratchBytes = kScratchBytes;
    if (!gApp.init(p)) return;
    for (;;) gApp.loop();
}
