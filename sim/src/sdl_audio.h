// OpenTracker simulator -- IAudioOut over SDL2 audio.
//
// The device codec runs at 44117.6 Hz (spec 01 §5: MCLK 11.294 MHz / 256);
// the simulator opens the host device at 44100 Hz, so pitch/tempo differ by
// 0.04 %. Blocks of 128 stereo int16 frames are pulled from the app callback
// on the SDL audio thread. Optional line-in (--line-in) captures the default
// recording device; captured blocks are exposed via lineIn(), which the app
// hands to the audio engine (HalRefs::lineIn).
#pragma once

#include <SDL.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "hal_api.h"

namespace sim {

// Master EQ / Bass Boost / Space (spec 18 §1) have no simulator DSP (the
// stock effect is the SGTL5000 DAP); the last values pushed are recorded.
struct MasterDspRecord {
    float eq[5] = {0, 0, 0, 0, 0};
    uint8_t bassBoost = 0, space = 0;
    uint32_t calls = 0;
    void set(const float* e, uint8_t b, uint8_t s) {
        for (int i = 0; i < 5; ++i) eq[i] = e[i];
        bassBoost = b;
        space = s;
        ++calls;
    }
};

class SdlAudio final : public ot::IAudioOut {
public:
    ~SdlAudio() override;
    void setCallback(FillFn fn, void* ctx) override {
        fn_ = fn;
        ctx_ = ctx;
    }
    bool start() override;
    void stop() override;
    void applyMasterDsp(const float eq[5], uint8_t bassBoost, uint8_t space) override {
        masterDsp.set(eq, bassBoost, space);
    }
    MasterDspRecord masterDsp;
    void enableLineIn(bool on) { lineInWanted_ = on; }
    // Latest captured block (128 interleaved frames) or nullptr; audio thread only.
    const int16_t* lineIn() const { return lineInValid_ ? lineInBlock_ : nullptr; }

private:
    static void sdlCallback(void* ud, Uint8* stream, int len);
    static void captureCallback(void* ud, Uint8* stream, int len);
    void fill(int16_t* out, int frames);

    FillFn fn_ = nullptr;
    void* ctx_ = nullptr;
    SDL_AudioDeviceID dev_ = 0, cap_ = 0;
    bool lineInWanted_ = false;
    int16_t block_[kBlockFrames * kChannels] = {};
    uint32_t blockPos_ = kBlockFrames;
    // capture ring (SPSC)
    static constexpr uint32_t kRing = 1u << 14;   // frames
    std::vector<int16_t> ring_ = std::vector<int16_t>(kRing * 2);
    std::atomic<uint32_t> wr_{0}, rd_{0};
    int16_t lineInBlock_[kBlockFrames * kChannels] = {};
    bool lineInValid_ = false;
};

// Headless: the script runner calls pull() for every 128 frames of virtual
// time; optionally records the output to a WAV file.
class ManualAudio final : public ot::IAudioOut {
public:
    ~ManualAudio() override;
    void setCallback(FillFn fn, void* ctx) override {
        fn_ = fn;
        ctx_ = ctx;
    }
    bool start() override { running_ = true; return true; }
    void stop() override { running_ = false; }
    void applyMasterDsp(const float eq[5], uint8_t bassBoost, uint8_t space) override {
        masterDsp.set(eq, bassBoost, space);
    }
    MasterDspRecord masterDsp;
    void pull();
    bool recordWav(const char* path);
    bool saveWav(const char* path);  // Save current buffer to new file, reset buffer

    // Audio level tracking for script assertions
    float peakLevel() const { return peak_; }
    float rmsLevel() const { return rms_; }
    bool hasAudio() const { return peak_ > 0.001f; }
    void resetLevels() { peak_ = 0.0f; rms_ = 0.0f; rmsAcc_ = 0.0f; rmsSamples_ = 0; audioBuffer_.clear(); }

private:
    FillFn fn_ = nullptr;
    void* ctx_ = nullptr;
    bool running_ = false;
    std::FILE* wav_ = nullptr;
    uint32_t wavBytes_ = 0;
    float peak_ = 0.0f;
    float rms_ = 0.0f;
    float rmsAcc_ = 0.0f;
    uint32_t rmsSamples_ = 0;
    std::vector<int16_t> audioBuffer_;  // For save_audio command
};

}  // namespace sim
