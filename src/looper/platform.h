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
    // One telemetry line a second (USB serial on the device); nullptr = none.
    void (*log)(const char* line) = nullptr;
    // SD card (FAT on the device, a host folder in the simulator): settings and
    // WAV exports. storageReady() false = no card.
    ot::IStorage* storage = nullptr;
    bool (*storageReady)() = nullptr;
    // Direct DMA between the SD card and SDRAM for the long-take streamer (null:
    // no such path, e.g. the simulator). sdDirectDma(on) returns whether it is on now.
    bool (*sdDirectDma)(bool on) = nullptr;
    // Headphone level 0..100 (device only; nullptr = not adjustable).
    void (*setHeadphones)(uint8_t level) = nullptr;
    const char* version = "dev";
    // Hand over to the update bootloader to flash /Firmware/<name>. On the device
    // it does not return when it works; false = refused (no such image, a name
    // the bootloader would not take, or the EEPROM flag cannot be set).
    bool (*firmwareUpdate)(const char* name) = nullptr;
    // MIDI (DIN and USB); nullptr = none. midiDinRealtime sends one real-time
    // byte (clock) on DIN straight away; the engine calls it from the audio
    // thread, so it must not block.
    ot::IMidiPort* midi = nullptr;
    void (*midiDinRealtime)(uint8_t b) = nullptr;

    // Loop memory (external SDRAM on the device), effect memory, and a staging buffer for
    // waveform bitmaps on their way to the display's RAM_G.
    uint8_t* loopMem = nullptr;
    uint32_t loopMemBytes = 0;
    uint8_t* fxMem = nullptr;        // looper::kFxMemBytes (delay and reverb lines), 4-byte aligned
    uint8_t* scratch = nullptr;
    uint32_t scratchBytes = 0;
};

}  // namespace looper
