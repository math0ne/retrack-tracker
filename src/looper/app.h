// Looper app: front-panel input, pad LEDs and the screen, around the Engine.
//
// Screen: four track lanes and, along the bottom edge, the labels of the eight
// screen keys F1..F8, each centred over its key as on the stock Tracker.
//   F1 Rec/Dub   F2 Play/Stop   F3 Mute   F4 Clear (Shift: all)     -- selected track
//   F5 Mode      opens the mode bar: the eight labels become the eight playback
//                modes, the key under one picks it (any other key cancels)
//   F6 Input     line / mic low / mic high   F7 Monitor on/off   F8 overdub feedback
// Other keys
//   Pads (4 rows x 12): row = track. Col 1 REC/DUB, col 2 PLAY/STOP, col 3 MUTE,
//                       col 4 CLEAR, cols 5..12 jump to that eighth of the loop
//   Rec / Play   record-overdub the selected track / transport start-stop
//   Delete       clear the selected track (Shift: all)   Up/Down  select track
//   Encoder      selected track volume (Shift: input gain)
//   Help         toggle the load / frame-rate readout
//   Master       mixer page on/off
//   I.Param      track options page on/off
//   Perform      performance mode: tap to latch it on (tap again for off), or
//                hold it (off on release). The waveforms show 8 effect cells
//                laid out like pad columns 5..12 of the track's row; a pad plays
//                its effect on that track while held, Shift + pad latches it.
//   Power        hold 1.5 s to switch off
//
// Track options page: a column per track over F1..F4 (stop: free / on bar /
// on beat, fixed length, count-in, input stereo / mono, what follows a take)
// and the tempo column over F5 (BPM, beats per bar, metronome, click level).
// Same navigation as the mixer; F8 goes back.
//
// Mixer page: seven strips, each over its screen key: tracks 1-4 (send 1,
// send 2, level), FX 1 delay (time, feedback, return), FX 2 reverb (size,
// tone, return) and master (level). F1..F7 or Left/Right pick a strip,
// Up/Down a row, the encoder sets it (Shift: fine). F8 goes back to the loops.
//
// Drawing goes straight to the FT81x display list (no widget layer). The
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
    const Engine& engine() const { return engine_; }   // read-only, for the simulator's checks

    // Bitmap geometry (public for the RAM_G layout check).
    static constexpr uint16_t kWaveW = 660, kWaveH = 88;
    static constexpr uint32_t kWaveBytes = uint32_t(kWaveW) * kWaveH;
    static constexpr uint32_t kWaveRamG = 0x090000;   // above the font image (ends 0x08C3E0)

private:
    static void audioFill(int16_t* out, uint32_t frames, void* ctx);
    void onInput(const ot::InputEvent& ev);
    void onFKey(int key);
    void onPad(int pad, bool down);
    void record(int t);
    void hint(int t, const char* msg, uint32_t now);
    void updateLeds(uint32_t now);
    void updateWaves(uint32_t now);
    void renderWave(int t, uint32_t span);
    void render(uint32_t now);

    // drawing helpers (count display-list words for the readout)
    void color(uint32_t rgb);
    void alpha(uint8_t a);
    void fillRect(int x0, int y0, int x1, int y1, int radius = 0);
    void hline(int x0, int x1, int y, int width16 = 16);
    void vline(int x, int y0, int y1, int width16 = 16);
    void dot(int x, int y, int radius);
    void text(int x, int y, uint8_t font, uint16_t opt, const char* s);
    void scissor(int x, int y, int w, int h);
    void scissorOff();

    void drawLane(int t, uint32_t now);
    void drawFKeys();
    void drawStats();

    // mixer page
    static constexpr int kStrips = 7;     // 4 tracks, delay, reverb, master
    static constexpr int kRows = 3;       // two small parameters, then the fader
    bool mixParam(int strip, int row, MixParam& p, int& track) const;
    void mixerKey(uint8_t id);
    void mixerEncoder(int delta);
    void drawMixer();
    void drawStrip(int s);

    // track options page
    static constexpr int kOptCols = 5;    // 4 tracks, tempo
    static constexpr int kOptRows = 5;
    bool optParam(int col, int row, OptParam& p, int& track) const;
    void optKey(uint8_t id);
    void optEncoder(int delta);
    void drawOptions();
    void drawOptColumn(int c);
    void drawGrid(int t, int wx, int wy, uint32_t span);

    Platform p_;
    Engine engine_;
    ot::IDisplay* d_ = nullptr;

    int selected_ = 0;
    bool shift_ = false;
    bool modeBar_ = false;       // F5 pressed: the F-key labels show the playback modes
    bool mixer_ = false;         // mixer page shown
    int strip_ = 0, row_ = 2;    // mixer selection
    bool options_ = false;       // track options page shown
    int optCol_ = 0, optRow_ = 0;
    // performance mode
    void closePerf();
    void drawPerfCells(int t, int wx, int wy);
    bool perf_ = false;
    bool perfToggleOff_ = false; // Perform pressed while on: off at release
    uint32_t perfDownMs_ = 0;
    uint8_t perfLatch_[kTracks] = {};   // latched effect bits per track
    bool stats_ = false;
    uint8_t source_ = 0;         // 0 line, 2 mic low, 3 mic high
    uint8_t inputGain_ = 50;
    uint32_t powerDownMs_ = 0;
    bool powerHeld_ = false;

    // a short message on one lane (e.g. why overdub is not available)
    int hintTrack_ = -1;
    const char* hintText_ = nullptr;
    uint32_t hintUntilMs_ = 0;

    // waveform cache per lane
    uint32_t waveVersion_[kTracks] = {};
    uint32_t waveSpan_[kTracks] = {};
    uint32_t waveDrawnMs_[kTracks] = {};
    bool waveValid_[kTracks] = {};

    float inMeter_[2] = {}, trackMeter_[kTracks] = {}, retMeter_[kFxSlots] = {}, outMeter_[2] = {};

    // frame pacing and stats
    uint32_t lastFrameMs_ = 0, lastLedMs_ = 0, fpsWindowMs_ = 0;
    uint32_t framesInWindow_ = 0, fps_ = 0;
    uint32_t words_ = 0, lastWords_ = 0, uploadBytes_ = 0, lastUploadBytes_ = 0;
    uint32_t cpuAvg_ = 0, cpuMax_ = 0, lastCpuMs_ = 0;
    bool haveCpu_ = false;
};

}  // namespace looper
