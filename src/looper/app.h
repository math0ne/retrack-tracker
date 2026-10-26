// Looper app: front-panel input, pad LEDs and the screen, around the Engine.
//
// Controls
//   Pads (4 rows x 12): row = track. Col 1 REC/DUB, col 2 PLAY/STOP, col 3 MUTE,
//                       col 4 CLEAR, cols 5..12 jump to that eighth of the loop
//                       (keeps the phase inside the eighth, so it stays in time).
//   Rec      record / overdub the selected track     Play   transport start/stop
//   Delete   clear the selected track (Shift: all)    Up/Down, F1..F4  select track
//   Encoder  selected track volume (Shift: input gain)
//   F5 monitor on/off   F6 input source   F7/F8 overdub feedback -/+
//   Power    hold 1.5 s to switch off
//
// Screen: drawn straight on the FT81x display list (no widget layer). The
// waveforms are RGB332 bitmaps rendered here and copied to RAM_G, so each lane
// costs a handful of display-list words however detailed it is.
#pragma once

#include <cstdint>

#include "looper/engine.h"
#include "looper/platform.h"

namespace looper {

class App {
public:
    bool init(const Platform& p);
    void loop();

    // Bitmap geometry (public for the RAM_G layout check).
    static constexpr uint16_t kWaveW = 600, kWaveH = 72;
    static constexpr uint32_t kWaveBytes = uint32_t(kWaveW) * kWaveH;
    static constexpr uint32_t kWaveRamG = 0x090000;   // above the font image (ends 0x08C3E0)

private:
    static void audioFill(int16_t* out, uint32_t frames, void* ctx);
    void onInput(const ot::InputEvent& ev);
    void onPad(int pad, bool down);
    void updateLeds(uint32_t now);
    void updateWaves(uint32_t now);
    void renderWave(int t, uint32_t span);
    void render(uint32_t now);

    // drawing helpers (count display-list words for the on-screen budget)
    void color(uint32_t rgb);
    void alpha(uint8_t a);
    void fillRect(int x0, int y0, int x1, int y1, int radius = 0);
    void hline(int x0, int x1, int y, int width16 = 16);
    void vline(int x, int y0, int y1, int width16 = 16);
    void dot(int x, int y, int radius);
    void text(int x, int y, uint8_t font, uint16_t opt, const char* s);
    void scissor(int x, int y, int w, int h);
    void scissorOff();

    void drawHeader(uint32_t now);
    void drawLane(int t, uint32_t now);
    void drawFooter();

    Platform p_;
    Engine engine_;
    ot::IDisplay* d_ = nullptr;
    bool fonts_ = false;

    int selected_ = 0;
    bool shift_ = false;
    uint8_t source_ = 0;         // 0 line, 2 mic low, 3 mic high
    uint8_t inputGain_ = 50;
    uint32_t powerDownMs_ = 0;
    bool powerHeld_ = false;

    // waveform cache per lane
    uint32_t waveVersion_[kTracks] = {};
    uint32_t waveSpan_[kTracks] = {};
    uint32_t waveDrawnMs_[kTracks] = {};
    bool waveValid_[kTracks] = {};

    // meters (decayed on the UI side)
    float inMeter_[2] = {}, outMeter_[2] = {}, trackMeter_[kTracks] = {};

    // frame pacing and stats
    uint32_t lastFrameMs_ = 0, lastLedMs_ = 0, fpsWindowMs_ = 0;
    uint32_t framesInWindow_ = 0, fps_ = 0;
    uint32_t words_ = 0, lastWords_ = 0, uploadBytes_ = 0, lastUploadBytes_ = 0;
    uint32_t cpuAvg_ = 0, cpuMax_ = 0, lastCpuMs_ = 0;
    bool haveCpu_ = false;
};

}  // namespace looper
