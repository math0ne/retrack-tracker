// Looper app: front-panel input, pad LEDs and the screen, around the Engine.
//
// Screen: four track lanes and, along the bottom edge, the labels of the eight
// screen keys F1..F8, each centred over its key as on the stock Tracker.
//   F1 Rec/Dub   F2 Play/Stop   F3 Mute                             -- selected track
//   F4 Tempo     tap: metronome on/off; hold + encoder: tempo (tape speed once there
//                is a loop: 50..200 %, the pitch follows); Shift + hold: click level
//   F5 Mode      opens the mode bar: the eight labels become the eight playback
//                modes, the key under one picks it (any other key cancels)
//   F6 Input     line / mic low / mic high   F8 overdub feedback
//   F7 Monitor   tap: on/off; hold + encoder: input gain (a knob shows while held)
// Other keys
//   Pads (4 rows x 12): row = track. Col 1 REC/DUB, col 2 PLAY/STOP, col 3 MUTE,
//                       col 4 CLEAR, cols 5..12 jump to that eighth of the loop
//   Rec / Play   record-overdub the selected track / transport start-stop
//   Delete       clear the selected track (Shift: all)   Up/Down  select track
//   Encoder      selected track volume (Shift: input gain)
//   Shift+Play   metronome on/off
//   Help         toggle the load / frame-rate readout
//   Shift+Help   stress preset: the heaviest audio load, for measuring on the device
//   Note         home: the loop lanes, from any page
//   Master       mixer page on/off
//   Instr        track options page on/off;  Song: the same page at the tempo column
//   FX1 / FX2    that effect slot's page on/off
//   Config       settings: display and pad brightness, headphones, input, monitor
//   File         projects (F1 save, F2 save new, F3 load) and WAV export (F5 track, F6 all)
//   S.Edit       loop editor for the selected track: F1/F2 start/end marker,
//                F3 snap, F4 trim, F5 cut, F6 erase (twice), F7 undo trim, F8 home
//   Perform      performance mode: tap to latch it on (tap again for off), or
//                hold it (off on release). The waveforms show 8 effect cells
//                laid out like pad columns 5..12 of the track's row; a pad plays
//                its effect on that track while held, Shift + pad latches it.
//                Turning the encoder while an effect pad is held sets that
//                effect's knob (shown over the lanes).
//   Power        hold 1.5 s to switch off
//
// Track options page: a column per track over F1..F4 (stop: free / on bar /
// on beat, fixed length, start: now / count-in / on sound, input stereo /
// mono, what follows a take) and the tempo column over F5 (BPM, beats per bar,
// metronome, click level, sound threshold). Same navigation as the mixer; F8 home.
//
// Mixer page: seven strips, each over its screen key: tracks 1-4 (send 1,
// send 2, level), FX 1 and FX 2 (their first two knobs and return) and master
// (level). F1..F7 or Left/Right pick a strip, Up/Down a row, the encoder sets
// it (Shift: fine). F8 home.
//
// FX page (FX1 / FX2): the effect's picture on top, its knobs below. F1 TYPE
// opens the type bar (the labels become the five effect types; the key under
// one picks it), F2..F5 pick a knob, F6 the return, F7 the other slot, F8 home.
// Left/Right also move between knobs; the encoder turns the selected one.
//
// Drawing goes straight to the FT81x display list (no widget layer). The
// waveforms are RGB332 bitmaps rendered here and copied to RAM_G, so each lane
// costs a handful of display-list words however detailed it is.
#pragma once

#include <cstdint>

#include "looper/engine.h"
#include "looper/files.h"
#include "looper/platform.h"

namespace looper {

class App {
public:
    bool init(const Platform& p);
    void loop();
    const Engine& engine() const { return engine_; }   // read-only, for the simulator's checks
    // Simulator scripts: set an option directly (as the options page would).
    void scriptOpt(int track, OptParam p, int value) {
        engine_.post(Engine::kOpt, uint8_t(track), int16_t((int(p) << 8) | (value & 0xFF)));
    }

    // Bitmap geometry (public for the RAM_G layout check).
    static constexpr uint16_t kWaveW = 660, kWaveH = 88;
    static constexpr uint32_t kWaveBytes = uint32_t(kWaveW) * kWaveH;
    static constexpr uint32_t kWaveRamG = 0x090000;   // above the font image (ends 0x08C3E0)
    static constexpr uint16_t kEditW = 760, kEditH = 160;   // the loop editor's waveform
    static constexpr uint32_t kEditRamG = 0x0C9000;   // after the four lane bitmaps

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
    void renderBitmap(int t, uint32_t span, int w, int h, uint32_t ramg);
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
    bool mixParam(int strip, int row, MixParam& p, int& track, int& fxKnob) const;
    void stepFxKnob(int slot, int knob, int delta);
    void fxKnobText(int slot, int knob, char* buf, size_t n) const;
    float fxKnobFrac(int slot, int knob) const;
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
    void drawGrid(int t, int wx, int wy, uint32_t span, int w = kWaveW, int h = kWaveH);

    // FX page
    static constexpr int kFxPageKnobs = kFxKnobs + 1;   // the type's knobs, then the return
    void goHome();
    void fxKey(uint8_t id);
    void fxEncoder(int delta);
    void drawFxPage(uint32_t now);
    void drawFxVisual(int slot, uint32_t now);
    void drawKnob(int cx, int cy, int r, float frac, float shown, uint32_t col, bool sel, float glow,
                  const char* name, const char* value);
    void arc(int cx, int cy, float r, float a0, float a1, int segs);   // a LINE_STRIP arc, radians
    int fxPage_ = -1;             // slot shown, -1 = none
    bool fxBar_ = false;          // F1: the labels show the effect types
    int fxSel_ = 0;               // selected knob, kFxKnobs = return
    float knobShown_[kFxSlots][kFxPageKnobs] = {};   // eased towards the values: the knobs glide

    // performance knob: hold an effect pad and turn the encoder
    void drawPerfKnob();
    void perfLabel(PerfFx fx, char* buf, size_t n) const;   // "Rpt 1/8" etc.
    void perfValue(PerfFx fx, char* buf, size_t n) const;
    int heldFx_ = -1;             // effect of the pad held last (or -1)
    uint8_t heldPads_[kTracks] = {};   // held effect pads per track
    float perfShown_ = 0.0f;
    // Loop editor (S.Edit): a big waveform of one track, start / end markers on
    // a snap grid, trim (non-destructive, undo), cut and erase (press twice).
    void editKey(uint8_t id);
    void editEncoder(int delta);
    void editReset();
    uint32_t editUnit(bool fine) const;
    void drawEditPage(uint32_t now);
    void fmtPos(char* buf, size_t n, uint32_t frames) const;   // bar.beat.16th from the loop start
    bool edit_ = false;
    int editTrack_ = 0;
    uint32_t editA_ = 0, editB_ = 0;   // selection, loop frames
    int editMarker_ = 0;               // 0 start, 1 end
    uint8_t editSnap_ = 1;             // 0 bar, 1 beat, 2 1/16, 3 free
    uint32_t editLen_ = 0, editStart_ = 0, editVersion_ = 0xFFFFFFFFu;   // what the bitmap shows
    bool editValid_ = false;
    int confirm_ = -1;                 // F-key waiting for its second press
    uint32_t confirmUntilMs_ = 0;

    // F7 on the home page: tap = monitor on/off, hold + encoder = input gain
    bool f7Held_ = false, f7Turned_ = false;
    float gainShown_ = 0.5f;
    // F4 on the home page: tap = metronome on/off, hold + encoder = tempo
    // (Shift: click level)
    bool f4Held_ = false, f4Turned_ = false;
    float clickShown_ = 0.6f, tempoShown_ = 0.5f;
    void tempoRange(int& lo, int& hi) const;   // the tempo the encoder can set: 50 .. 200 % of a loop's
    void drawPopupKnob(const char* title, const char* value, float frac, float& shown, uint32_t col, float meter);

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
    // Settings kept on the card (Config page; input source and gain too)
    Files files_;
    Config cfg_;
    bool cfgDirty_ = false;
    uint32_t cfgDirtyMs_ = 0;
    void markConfig();           // saved a second after the last change
    void applyConfig();
    // Config and File pages
    bool config_ = false, file_ = false;
    int cfgRow_ = 0, projSel_ = 0;
    void configKey(uint8_t id);
    void configEncoder(int delta);
    void drawConfig();
    void fileKey(uint8_t id);
    void drawFile();
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
    int waveNext_ = 0;           // lane that gets the next upload turn
    uint32_t waveUploads_ = 0, lastWaveUploads_ = 0;   // per second, for the telemetry

    float inMeter_[2] = {}, trackMeter_[kTracks] = {}, retMeter_[kFxSlots] = {}, outMeter_[2] = {};

    // frame pacing and stats
    uint32_t lastFrameMs_ = 0, lastLedMs_ = 0, fpsWindowMs_ = 0;
    uint32_t framesInWindow_ = 0, fps_ = 0;
    uint32_t words_ = 0, lastWords_ = 0, uploadBytes_ = 0, lastUploadBytes_ = 0;
    uint32_t cpuAvg_ = 0, cpuMax_ = 0, lastCpuMs_ = 0;
    bool haveCpu_ = false;
    uint32_t loopMaxUs_ = 0, loopsInWindow_ = 0;   // main-loop pass times over the telemetry second
    void telemetry();
    void stressPreset();
};

}  // namespace looper
