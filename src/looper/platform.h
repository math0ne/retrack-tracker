// What the looper app needs from the outside world. The device (src/device_main.cpp)
// fills it from the OpenTracker HAL; the simulator (sim/src/main.cpp) from its
// SDL/headless stand-ins. Nothing in src/looper includes a hal/ header.
#pragma once

#include <cstdint>

#include "hal_api.h"

namespace looper {

struct Platform {
    ot::IDisplay* display = nullptr;
    ot::IAudioOut* audio = nullptr;
    ot::IInput* input = nullptr;
    ot::ILeds* leds = nullptr;
    ot::IClock* clock = nullptr;

    // Latest 128-frame interleaved input block, called from the audio callback;
    // nullptr when no input is available.
    const int16_t* (*lineIn)(void* ctx) = nullptr;
    void* lineInCtx = nullptr;
    // Codec input mux: 0 line in, 1 FM radio, 2 mic low gain, 3 mic high gain; gain 0..100.
    bool (*selectInput)(uint8_t source, uint8_t gain) = nullptr;
    void (*service)() = nullptr;     // HAL housekeeping, once per loop()
    void (*powerOff)() = nullptr;    // never returns on the device
    // Audio render load over the last second, in permille of the block budget.
    bool (*cpuLoad)(uint32_t& avgPermille, uint32_t& maxPermille) = nullptr;
    // Bytes of the last display list the display controller ran (device only).
    uint32_t (*dlBytes)() = nullptr;

    // Loop memory (external SDRAM on the device) and a staging buffer for
    // waveform bitmaps on their way to the display's RAM_G.
    uint8_t* loopMem = nullptr;
    uint32_t loopMemBytes = 0;
    uint8_t* scratch = nullptr;
    uint32_t scratchBytes = 0;
};

}  // namespace looper
