#include "looper/app.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "ui/buttons.h"
#include "ui/fonts/fonts.h"

namespace looper {

namespace {

namespace b = ot::btn;
using Disp = ot::IDisplay;

static_assert(App::kWaveRamG >= ot::fontdata::kRamGEnd, "waveforms overlap the font image");
static_assert(App::kWaveRamG + kTracks * App::kWaveBytes <= Disp::kRamGSize, "waveforms leave RAM_G");
static_assert(App::kWaveBytes % 4 == 0, "RAM_G writes stay 4-byte aligned");
static_assert(App::kEditRamG >= App::kWaveRamG + kTracks * App::kWaveBytes, "editor bitmap overlaps the lanes");
static_assert(App::kEditRamG + uint32_t(App::kEditW) * App::kEditH <= Disp::kRamGSize, "editor bitmap leaves RAM_G");

constexpr uint32_t kFrameMs = 16;           // ~60 fps cap
constexpr uint32_t kLedMs = 33;
constexpr uint32_t kRecWaveMs = 50;         // recording lanes redraw at 20 Hz (SPI budget)
constexpr uint32_t kPowerHoldMs = 1500;
constexpr uint32_t kHintMs = 1500;

// Geometry: four lanes, then the F-key label row along the bottom edge.
constexpr int kLaneX0 = 6, kLaneX1 = 794, kLaneY0 = 6, kLaneH = 100, kLaneGap = 6;
constexpr int kWaveX = 14, kWaveDy = 6;   // the waveform fills the lane
constexpr int kKeyY0 = 436, kKeyY1 = 476, kKeyW = 100;   // F1..F8 cells, each over its key

// Palette: greyscale. color() turns anything else to its grey too (by
// luminance), so the accents below are just brighter or darker greys.
constexpr uint32_t kBg = 0x101010, kLane = 0x1F1F1F, kLaneSel = 0x2B2B2B, kCell = 0x262626;
constexpr uint32_t kInk = 0xEEEEEE, kDim = 0x828282, kFaint = 0x3A3A3A;
constexpr uint32_t kRed = 0xFFFFFF, kAmber = 0xC6C6C6;   // recording = the brightest; the page accent
constexpr uint32_t kTrackColor[kTracks] = {0xF4F4F4, 0xDCDCDC, 0xC4C4C4, 0xACACAC};

constexpr uint8_t kFontSmall = 26, kFontBody = 27, kFontBig = 28;
constexpr uint8_t kFeedbackSteps[] = {100, 90, 75, 50};

inline uint8_t r8(uint32_t c) { return uint8_t(c >> 16); }
inline uint8_t g8(uint32_t c) { return uint8_t(c >> 8); }
inline uint8_t b8(uint32_t c) { return uint8_t(c); }

uint32_t scale(uint32_t c, float k) {
    if (k < 0) k = 0;
    if (k > 1) k = 1;
    return (uint32_t(r8(c) * k) << 16) | (uint32_t(g8(c) * k) << 8) | uint32_t(b8(c) * k);
}

// Each channel raised by d, clamped (lit edges).
uint32_t lighten(uint32_t c, int d) {
    auto ch = [&](int v) { return uint32_t(v + d > 255 ? 255 : v + d); };
    return (ch(r8(c)) << 16) | (ch(g8(c)) << 8) | ch(b8(c));
}

// 0..1 triangle pulse with the given period.
float pulse(uint32_t now, uint32_t periodMs) {
    const float ph = float(now % periodMs) / float(periodMs);
    return ph < 0.5f ? ph * 2.0f : (1.0f - ph) * 2.0f;
}

void fmtSeconds(char* buf, size_t n, uint32_t frames) {
    const uint32_t cs = uint32_t(uint64_t(frames) * 100 / kSampleRate);
    std::snprintf(buf, n, "%lu.%02lu s", static_cast<unsigned long>(cs / 100), static_cast<unsigned long>(cs % 100));
}

// Mixer strip colours: the four tracks, delay, reverb, master, back.
constexpr uint32_t kDelayColor = 0xB388FF, kReverbColor = 0x64D2FF;
uint32_t stripColor(int s) {
    if (s < kTracks) return kTrackColor[s];
    if (s == 4) return kDelayColor;
    if (s == 5) return kReverbColor;
    return s == 6 ? kInk : kDim;
}

const char* sourceName(uint8_t s) {
    switch (s) {
        case 0: return "Line";
        case 2: return "Mic low";
        case 3: return "Mic high";
    }
    return "?";
}

}  // namespace

bool App::init(const Platform& p) {
    p_ = p;
    d_ = p.display;
    if (!d_ || !p_.audio || !p_.input || !p_.leds || !p_.clock || !p_.loopMem || !p_.fxMem) return false;
    if (!p_.scratch || p_.scratchBytes < uint32_t(kEditW) * 16) return false;   // bitmaps go up in bands of rows

    // Split the loop memory into four tracks: frames (4 B) plus two peak bytes
    // (left, right) per kPeakFrames.
    TrackMemory mem[kTracks];
    const uint32_t perTrack = (p_.loopMemBytes / kTracks) & ~15u;
    const uint32_t frames = uint32_t(uint64_t(perTrack) * kPeakFrames / (4u * kPeakFrames + 2u)) / kPeakFrames *
                            kPeakFrames;
    for (int t = 0; t < kTracks; ++t) {
        uint8_t* base = p_.loopMem + size_t(t) * perTrack;
        mem[t].frames = reinterpret_cast<int16_t*>(base);
        mem[t].capacity = frames;
        mem[t].peaks = base + size_t(frames) * 4;
    }
    engine_.init(mem, p_.fxMem);

    d_->bindFontSet(0);
    files_.init(&engine_, &p_);
    files_.loadConfig(cfg_);   // defaults when there is no card or no file yet
    midi_.init(&p_, &engine_);
    engine_.setRealtimeHook(p_.midiDinRealtime);
    applyConfig();
    p_.audio->setCallback(&App::audioFill, this);
    return p_.audio->start();
}

void App::audioFill(int16_t* out, uint32_t frames, void* ctx) {
    App* a = static_cast<App*>(ctx);
    const int16_t* in = a->p_.lineIn ? a->p_.lineIn(a->p_.lineInCtx) : nullptr;
    a->engine_.render(out, in, frames);
}

void App::loop() {
    const uint32_t t0 = p_.clock->micros();
    if (p_.service) p_.service();
    ot::InputEvent ev;
    while (p_.input->poll(ev)) onInput(ev);
    midi_.service(MidiClock(cfg_.midiClock), cfg_.midiMap, p_.clock->micros());
    MidiAction ma;
    while (midi_.nextAction(ma)) midiAction(ma);
    {
        MidiBinding nb;
        int na;
        if (midi_.learned(nb, na)) {   // Learn on the MIDI page: this note / controller now does that
            for (MidiBinding& o : cfg_.midiMap)   // one action per message
                if (o.type == nb.type && o.num == nb.num) o = MidiBinding{};
            cfg_.midiMap[na] = nb;
            markConfig();
        }
    }
    engine_.service();
    files_.service();   // one step of a save / load / export

    const uint32_t now = p_.clock->millis();
    if (powerHeld_ && now - powerDownMs_ >= kPowerHoldMs && p_.powerOff) {
        p_.leds->clear();
        p_.leds->show();
        p_.powerOff();
    }
    // Settings go to the card a second after the last change (one write per edit burst).
    if (cfgDirty_ && now - cfgDirtyMs_ >= 1000 && !files_.busy()) {
        cfgDirty_ = false;
        files_.saveConfig(cfg_);
    }
    if (now - lastLedMs_ >= kLedMs) {
        lastLedMs_ = now;
        updateLeds(now);
    }
    if (now - lastCpuMs_ >= 1000) {
        lastCpuMs_ = now;
        if (p_.cpuLoad) haveCpu_ = p_.cpuLoad(cpuAvg_, cpuMax_);
        telemetry();
    }
    if (now - lastFrameMs_ >= kFrameMs && d_->frameReady()) {
        lastFrameMs_ = now;
        if (edit_) {
            // The editor's big waveform: redrawn when the loop changes (trim, cut,
            // erase, overdub), the lane bitmaps wait (one upload per frame).
            const TrackView& v = engine_.view(editTrack_);
            const uint32_t len = v.len.load(), start = v.start.load(), ver = v.peakVersion.load();
            if (len && (!editValid_ || len != editLen_ || start != editStart_ ||
                        (ver != editVersion_ && now - waveDrawnMs_[editTrack_] >= 250))) {   // 121 KB: ~33 ms of SPI
                if (len != editLen_ || start != editStart_) editReset();
                renderBitmap(editTrack_, len, kEditW, kEditH, kEditRamG);
                editLen_ = len;
                editStart_ = start;
                editVersion_ = ver;
                editValid_ = true;
                waveDrawnMs_[editTrack_] = now;
                waveValid_[editTrack_] = false;   // the lane redraws when it is back on screen
            }
        } else if (!mixer_ && !options_ && fxPage_ < 0 && !config_ && !file_ && !inPage_) {
            updateWaves(now);   // lanes only when they are on screen
        }
        render(now);
    }
    // The longest pass is the worst wait a pad press can see before it is read.
    const uint32_t dt = p_.clock->micros() - t0;
    if (dt > loopMaxUs_) loopMaxUs_ = dt;
    ++loopsInWindow_;
}

void App::telemetry() {
    // One line a second: audio interrupt load (of the 2.9 ms block), the longest
    // main-loop pass, frame rate, display list, SPI uploads, track states.
    if (p_.log) {
        static const char kState[] = "-ECRPDS";   // clearing, empty, count-in, rec, play, dub, stop
        char st[kTracks + 1];
        for (int t = 0; t < kTracks; ++t) st[t] = kState[engine_.view(t).state.load() % 7];
        st[kTracks] = 0;
        const uint32_t dl = p_.dlBytes ? p_.dlBytes() : lastWords_ * 4;
        char line[200];
        std::snprintf(line, sizeof line,
                      "looper: cpu avg %lu.%lu%% max %lu.%lu%% | loop max %lu us (%lu/s) | %lu fps | dl %lu B | "
                      "spi %lu KB/s, %lu waves/s | tracks %s perf %02x%02x%02x%02x",
                      static_cast<unsigned long>(cpuAvg_ / 10), static_cast<unsigned long>(cpuAvg_ % 10),
                      static_cast<unsigned long>(cpuMax_ / 10), static_cast<unsigned long>(cpuMax_ % 10),
                      static_cast<unsigned long>(loopMaxUs_), static_cast<unsigned long>(loopsInWindow_),
                      static_cast<unsigned long>(fps_), static_cast<unsigned long>(dl),
                      static_cast<unsigned long>(lastUploadBytes_ / 1024), static_cast<unsigned long>(lastWaveUploads_),
                      st, unsigned(engine_.view(0).perf.load()), unsigned(engine_.view(1).perf.load()),
                      unsigned(engine_.view(2).perf.load()), unsigned(engine_.view(3).perf.load()));
        p_.log(line);
    }
    loopMaxUs_ = 0;
    loopsInWindow_ = 0;
}

// ---------------------------------------------------------------------------
// Input

void App::hint(int t, const char* msg, uint32_t now) {
    hintTrack_ = t;
    hintText_ = msg;
    hintUntilMs_ = now + kHintMs;
}

void App::record(int t, bool replace) {
    const TrackState s = TrackState(engine_.view(t).state.load());
    const ModeInfo& mi = modeInfo(engine_.mode(t));
    if (s == TrackState::kPlaying && !mi.canDub) {
        hint(t, "Overdub needs Forward, Reverse or Ping-pong", p_.clock->millis());
        return;
    }
    // Shift: replace (punch-in) instead of overdub, on a track with a loop.
    const bool repl = (shift_ || replace) &&
                      (s == TrackState::kPlaying || s == TrackState::kStopped || s == TrackState::kQueued);
    engine_.post(Engine::kRecord, uint8_t(t), int16_t(repl ? 1 : 0));
}

namespace {
// The mode bar: two pages of the eight keys, F5 turns the page (-1), -2 = blank.
constexpr int kModeMore = -1, kModeNone = -2;
constexpr int kModeBarKeys[2][8] = {
    {int(PlayMode::kForward), int(PlayMode::kReverse), int(PlayMode::kPingPong), int(PlayMode::kHalfSpeed),
     kModeMore, int(PlayMode::kDoubleSpeed), int(PlayMode::kStretch), int(PlayMode::kOctaveUp)},
    {int(PlayMode::kOctaveDown), int(PlayMode::kGranular), kModeNone, kModeNone, kModeMore, kModeNone, kModeNone,
     kModeNone},
};
}  // namespace

void App::onFKey(int key) {
    const uint8_t t = uint8_t(selected_);
    if (modeBar_) {   // the key under a mode label picks it; F5 turns the page
        const int m = kModeBarKeys[modePage_ & 1][key & 7];
        if (m == kModeMore) {
            modePage_ ^= 1;
            return;
        }
        if (m >= 0) engine_.post(Engine::kMode, t, int16_t(m));
        modeBar_ = false;
        return;
    }
    switch (key) {
        case 0: record(selected_); break;
        case 1: engine_.post(Engine::kPlayStop, t); break;
        case 2: engine_.post(Engine::kMute, t); break;
        // F4 (metronome / tempo), F5 (mode), F6 (input) and F7 (volume) are tap / hold keys: onInput
        default: break;
    }
}

void App::undoDub(bool redo) {
    const TrackView& v = engine_.view(selected_);
    const uint8_t u = v.dubUndo.load();
    const uint32_t now = p_.clock->millis();
    if (!v.len.load()) return;
    if (redo ? u != 2 : u != 1) {
        const uint32_t cap = engine_.memory(selected_).capacity * (v.mono.load() ? 2u : 1u);
        hint(selected_, v.start.load() + 2 * v.len.load() > cap ? "Too long to undo (over half the memory)"
                        : redo ? "Nothing to redo" : "Nothing to undo", now);
        return;
    }
    engine_.post(Engine::kDubUndo, uint8_t(selected_), int16_t(redo ? 1 : 0));
    hint(selected_, redo ? "Redo" : "Undo: the overdub is gone (redo brings it back)", now);
}

void App::multiply(bool halve) {
    const TrackView& v = engine_.view(selected_);
    const uint32_t len = v.len.load();
    if (!len) return;
    const uint32_t cap = engine_.memory(selected_).capacity * (v.mono.load() ? 2u : 1u);
    if (!halve && v.start.load() + 2 * len > cap) {
        hint(selected_, "Too long to double", p_.clock->millis());
        return;
    }
    engine_.post(Engine::kMultiply, uint8_t(selected_), int16_t(halve ? 0 : 1));
    hint(selected_, halve ? "Halved" : "Doubled: it plays twice, overdub the copy", p_.clock->millis());
}

void App::shiftFKey(int key) {
    const uint32_t now = p_.clock->millis();
    switch (key) {
        case 0: record(selected_); break;   // Shift: replace
        case 1:   // every track from bar 1, or all stopped (the transport)
            engine_.post(Engine::kTransport);
            break;
        case 2: {   // solo the selected track; again: everything back
            if (solo_ == selected_) {
                for (int t = 0; t < kTracks; ++t) engine_.post(Engine::kSetMute, uint8_t(t), 0);
                solo_ = -1;
                hint(selected_, "Solo off", now);
            } else {
                for (int t = 0; t < kTracks; ++t) engine_.post(Engine::kSetMute, uint8_t(t), t == selected_ ? 0 : 1);
                solo_ = selected_;
                hint(selected_, "Solo", now);
            }
            break;
        }
        case 4: undoDub(false); break;
        case 5: undoDub(true); break;
        case 6: multiply(false); break;
        case 7: multiply(true); break;
        default: break;
    }
}

void App::tapTempo(uint32_t ms) {
    // Taps more than 2 s apart start over; from the third tap on, the tempo is
    // the average of the last (up to four) intervals.
    if (taps_ && ms - tapMs_[taps_ - 1] > 2000) taps_ = 0;
    if (taps_ == 5) {
        for (int i = 0; i < 4; ++i) tapMs_[i] = tapMs_[i + 1];
        taps_ = 4;
    }
    tapMs_[taps_++] = ms;
    if (taps_ < 3) {
        hint(selected_, "Tap tempo: keep tapping", ms);
        return;
    }
    const float avg = float(tapMs_[taps_ - 1] - tapMs_[0]) / float(taps_ - 1);
    int lo, hi;
    tempoRange(lo, hi);
    int bpm = int(std::lround(60000.0f / (avg > 1.0f ? avg : 1.0f)));
    bpm = bpm < lo ? lo : (bpm > hi ? hi : bpm);
    engine_.post(Engine::kOpt, 0, int16_t((int(OptParam::kTempo) << 8) | bpm));
    static char buf[32];
    std::snprintf(buf, sizeof buf, "Tap tempo: %d bpm", bpm);
    hint(selected_, buf, ms);
}

void App::tempoRange(int& lo, int& hi) const {
    if (!engine_.masterLen()) {
        lo = 40;
        hi = 240;
        return;
    }
    const float b = engine_.baseTempo();
    lo = int(std::ceil(b * 0.5f));
    hi = int(std::floor(b * 2.0f));
    if (lo < 1) lo = 1;
    if (hi > 255) hi = 255;
}

void App::onInput(const ot::InputEvent& ev) {
    using E = ot::InputEvent;
    if (ev.type == E::kPadDown || ev.type == E::kPadUp) {
        onPad(ev.id, ev.type == E::kPadDown);
        return;
    }
    if (ev.type == E::kEncoder) {
        if (grainPanel()) {   // F5 held on a Granular track: the selected grain knob
            const GrainKnob& gk = kGrainKnobInfo[grainSel_];
            int v = int(engine_.grainParam(selected_, grainSel_)) + ev.delta * (grainSel_ == 3 || shift_ ? 1 : 2);
            v = v < 0 ? 0 : (v > gk.max ? gk.max : v);
            engine_.post(Engine::kGrainParam, uint8_t(selected_), int16_t((grainSel_ << 8) | v));
            f5Used_ = true;
            return;
        }
        if (f7Held_) {   // F7 held: the selected track's volume
            const int vol = int(engine_.volume(selected_)) + ev.delta * (shift_ ? 1 : 2);
            engine_.post(Engine::kVolume, uint8_t(selected_), int16_t(vol < 0 ? 0 : (vol > 100 ? 100 : vol)));
            return;
        }
        if (f6Held_) {   // F6 held: input gain
            int g = int(cfg_.gain) + ev.delta * (shift_ ? 1 : 2);
            cfg_.gain = uint8_t(g < 0 ? 0 : (g > 100 ? 100 : g));
            if (p_.selectInput) p_.selectInput(cfg_.source, cfg_.gain);
            f6Turned_ = true;
            markConfig();
            return;
        }
        if (f4Held_) {   // F4 held: tempo (tape speed once there is a loop); Shift: click level
            if (shift_) {
                int c = int(engine_.opt(OptParam::kClick)) + ev.delta * 5;
                c = c < 0 ? 0 : (c > 100 ? 100 : c);
                engine_.post(Engine::kOpt, 0, int16_t((int(OptParam::kClick) << 8) | c));
            } else {
                int lo, hi;
                tempoRange(lo, hi);
                int v = int(engine_.opt(OptParam::kTempo)) + ev.delta;
                v = v < lo ? lo : (v > hi ? hi : v);
                engine_.post(Engine::kOpt, 0, int16_t((int(OptParam::kTempo) << 8) | v));
            }
            f4Turned_ = true;
            return;
        }
        if (perf_ && heldFx_ >= 0) {
            // An effect pad is held: the encoder turns that effect's knob.
            const PerfKnob& kn = kPerfKnobs[heldFx_];
            const int step = kn.list || shift_ ? 1 : 2;
            int v = int(engine_.perfParam(PerfFx(heldFx_))) + ev.delta * step;
            v = v < 0 ? 0 : (v > kn.max ? kn.max : v);
            engine_.post(Engine::kPerfParam, 0, int16_t((heldFx_ << 8) | v));
            return;
        }
        if (fxPage_ >= 0) {
            fxEncoder(ev.delta);
            return;
        }
        if (config_) {
            configEncoder(ev.delta);
            return;
        }
        if (inPage_) {
            inputEncoder(ev.delta);
            return;
        }
        if (edit_) {
            editEncoder(ev.delta);
            return;
        }
        if (mixer_) {
            mixerEncoder(ev.delta);
            return;
        }
        if (options_) {
            optEncoder(ev.delta);
            return;
        }
        if (shift_) {
            int g = int(cfg_.gain) + ev.delta * 2;
            cfg_.gain = uint8_t(g < 0 ? 0 : (g > 100 ? 100 : g));
            if (p_.selectInput) p_.selectInput(cfg_.source, cfg_.gain);
        } else {
            int v = int(engine_.volume(selected_)) + ev.delta * 2;
            engine_.post(Engine::kVolume, uint8_t(selected_), int16_t(v));
        }
        return;
    }
    const bool down = ev.type == E::kButtonDown;
    if (ev.id == b::Shift) shift_ = down;
    if (ev.id == b::Power) {
        powerHeld_ = down;
        powerDownMs_ = ev.timeMs;
    }
    if (ev.id == b::Performance || ev.id == b::Pattern) {   // Pattern is a second Perform key
        // Tap: latch on (a second tap turns it off). Hold: on until released.
        if (down) {
            perfToggleOff_ = perf_;
            perf_ = true;
            perfDownMs_ = ev.timeMs;
            goHome();   // the effect cells sit on the lanes
        } else if (perfToggleOff_ || ev.timeMs - perfDownMs_ >= 400) {
            closePerf();
        }
        return;
    }
    const bool home = !mixer_ && !options_ && fxPage_ < 0 && !edit_ && !config_ && !file_ && !inPage_ && !modeBar_;
    if (home && shift_ && b::isFKey(ev.id) && ev.id != b::F4 && !f5Held_ && !f6Held_ && !f7Held_) {
        if (down) shiftFKey(ev.id - b::F1);
        return;
    }
    if (ev.id == b::EncPush) {   // tap tempo, on any page
        if (down) tapTempo(ev.timeMs);
        return;
    }
    // Home page F5: a tap opens the mode bar; held on a Granular track, the
    // grain knobs (F1..F4 pick one while it is held).
    if (ev.id == b::F5 && (home || f5Held_)) {
        if (down) {
            f5Held_ = true;
            f5Used_ = false;
            f5DownMs_ = ev.timeMs;
        } else if (f5Held_) {
            f5Held_ = false;
            if (!f5Used_ && ev.timeMs - f5DownMs_ < kTapMs) {
                modeBar_ = true;
                modePage_ = engine_.mode(selected_) == PlayMode::kOctaveDown ||
                                    engine_.mode(selected_) == PlayMode::kGranular
                                ? 1 : 0;   // the page with the track's mode
            }
        }
        return;
    }
    if (f5Held_ && down && ev.id >= b::F1 && ev.id <= b::F4 &&
        engine_.mode(selected_) == PlayMode::kGranular) {
        grainSel_ = ev.id - b::F1;
        f5Used_ = true;
        return;
    }
    if (f5Held_ && ev.id >= b::F1 && ev.id <= b::F4) return;   // their release too
    // Home page F7: hold + encoder sets the selected track's volume (the
    // encoder alone does too; holding shows the knob).
    if (ev.id == b::F7 && (home || f7Held_)) {
        f7Held_ = down;
        return;
    }
    // Home page F6: a tap opens the Input page; held, the encoder sets the input gain.
    if (ev.id == b::F6 && (home || f6Held_)) {
        if (down) {
            f6Held_ = true;
            f6Turned_ = false;
            f6DownMs_ = ev.timeMs;
        } else if (f6Held_) {
            f6Held_ = false;
            if (!f6Turned_ && ev.timeMs - f6DownMs_ < kTapMs) {   // a tap, not a hold
                inPage_ = true;
                inCol_ = inRow_ = 0;
            }
        }
        return;
    }
    // Home page F4: a tap turns the metronome on / off; held, the encoder sets the click level.
    if (ev.id == b::F4 && (home || f4Held_)) {
        if (down) {
            f4Held_ = true;
            f4Turned_ = false;
            f4DownMs_ = ev.timeMs;
        } else if (f4Held_) {
            f4Held_ = false;
            if (!f4Turned_ && ev.timeMs - f4DownMs_ < kTapMs) {   // a tap, not a hold
                const bool on = !engine_.opt(OptParam::kMetronome);
                engine_.post(Engine::kOpt, 0, int16_t((int(OptParam::kMetronome) << 8) | (on ? 1 : 0)));
            }
        }
        return;
    }
    if (!down) return;
    if (ev.id == b::Note) {   // home: the lanes
        goHome();
        return;
    }
    if (ev.id == b::Master || ev.id == b::Instr || ev.id == b::Song) {   // page keys toggle their page
        const bool wasMixer = mixer_, wasOptions = options_;
        goHome();
        if (ev.id == b::Master) mixer_ = !wasMixer;
        else if (ev.id == b::Instr) options_ = !wasOptions;
        else {   // Song: straight to the tempo column
            options_ = true;
            optCol_ = 4;
            optRow_ = 0;
        }
        return;
    }
    if (ev.id == b::Config || ev.id == b::File) {   // settings, projects and export
        const bool wasConfig = config_, wasFile = file_;
        goHome();
        if (ev.id == b::Config) config_ = !wasConfig;
        else if (!wasFile) {
            file_ = true;
            files_.rescan();
            // Start on the current project (or the newest).
            projSel_ = files_.projectCount() - 1;
            for (int i = 0; i < files_.projectCount(); ++i)
                if (files_.projectNumber(i) == files_.current()) projSel_ = i;
            if (projSel_ < 0) projSel_ = 0;
        }
        return;
    }
    if (ev.id == b::SampleEditor) {   // S.Edit: the loop editor for the selected track
        const bool was = edit_;
        goHome();
        if (!was) {
            edit_ = true;
            editTrack_ = selected_;
            editReset();
        }
        return;
    }
    if (ev.id == b::Fx1 || ev.id == b::Fx2) {   // that slot's FX page
        const int s = ev.id == b::Fx1 ? 0 : 1;
        const bool same = fxPage_ == s;
        goHome();
        if (!same) {
            fxPage_ = s;
            fxSel_ = 0;
        }
        return;
    }
    const bool nav = b::isFKey(ev.id) || ev.id == b::Left || ev.id == b::Right || ev.id == b::Up || ev.id == b::Down;
    if (fxPage_ >= 0 && nav) {
        fxKey(ev.id);
        return;
    }
    if (config_ && nav) {
        configKey(ev.id);
        return;
    }
    if (inPage_ && nav) {
        inputKey(ev.id);
        return;
    }
    if (file_ && nav) {
        fileKey(ev.id);
        return;
    }
    if (edit_ && nav) {
        editKey(ev.id);
        return;
    }
    if (mixer_ && nav) {
        mixerKey(ev.id);
        return;
    }
    if (options_ && nav) {
        optKey(ev.id);
        return;
    }
    if (b::isFKey(ev.id)) {
        onFKey(ev.id - b::F1);
        return;
    }
    if (modeBar_ && ev.id != b::Shift && ev.id != b::Power) {   // any other key closes the mode bar
        modeBar_ = false;
        return;
    }
    switch (ev.id) {
        case b::Insert: undoDub(shift_); break;     // undo the last overdub / replace (Shift: redo it)
        case b::Copy: multiply(shift_); break;      // double the loop (Shift: halve it)
        case b::Rec: record(selected_); break;
        case b::Play:
            if (shift_) {   // Shift + Play: metronome on / off
                const bool on = !engine_.opt(OptParam::kMetronome);
                engine_.post(Engine::kOpt, 0, int16_t((int(OptParam::kMetronome) << 8) | (on ? 1 : 0)));
                hint(selected_, on ? "Metronome on" : "Metronome off", p_.clock->millis());
            } else {
                engine_.post(Engine::kTransport);
            }
            break;
        case b::Delete:
            engine_.post(shift_ ? Engine::kClearAll : Engine::kClear, uint8_t(selected_));
            break;
        case b::Up: selected_ = (selected_ + kTracks - 1) % kTracks; break;
        case b::Down: selected_ = (selected_ + 1) % kTracks; break;
        case b::Help:
            if (shift_) stressPreset();
            else stats_ = !stats_;
            break;
        default: break;
    }
}

void App::stressPreset() {
    // Shift + Help: the heaviest audio load the app can make, for measuring the
    // ceiling on the device. Every track with a loop goes to Granular at full
    // density, an octave up (each grain reads twice the frames), with both
    // filters latched and full sends; FX 1 a reverb, FX 2 Clouds at full
    // density; metronome and the readout on. Tap Perform twice to drop the effects.
    for (int t = 0; t < kTracks; ++t) {
        if (!engine_.view(t).len.load()) continue;
        engine_.post(Engine::kMode, uint8_t(t), int16_t(PlayMode::kGranular));
        engine_.post(Engine::kGrainParam, uint8_t(t), int16_t((1 << 8) | 100));                   // size
        engine_.post(Engine::kGrainParam, uint8_t(t), int16_t((2 << 8) | 100));                   // density
        engine_.post(Engine::kGrainParam, uint8_t(t), int16_t((3 << 8) | (kGrainPitchZero + 12)));
        static const PerfFx kStressFx[] = {PerfFx::kLowPass, PerfFx::kHighPass};
        for (PerfFx fx : kStressFx) {
            perfLatch_[t] |= uint8_t(1u << int(fx));
            engine_.post(Engine::kPerf, uint8_t(t), int16_t((int(fx) << 8) | 1));
        }
        engine_.post(Engine::kMix, uint8_t(t), int16_t((int(MixParam::kSend1) << 8) | 100));
        engine_.post(Engine::kMix, uint8_t(t), int16_t((int(MixParam::kSend2) << 8) | 100));
    }
    engine_.post(Engine::kFxType, 0, int16_t(FxType::kReverb));
    engine_.post(Engine::kFxType, 1, int16_t(FxType::kClouds));
    engine_.post(Engine::kFxKnob, 1, int16_t((3 << 8) | 100));   // Clouds density: every grain
    engine_.post(Engine::kOpt, 0, int16_t((int(OptParam::kMetronome) << 8) | 1));
    engine_.post(Engine::kMonitor, 0, 1);
    stats_ = true;
    hint(selected_, "Stress: granular, filters, reverb + Clouds, click", p_.clock->millis());
}

void App::closePerf() {
    perf_ = false;
    perfToggleOff_ = false;
    for (uint8_t& l : perfLatch_) l = 0;
    for (uint8_t& h : heldPads_) h = 0;
    heldFx_ = -1;
    engine_.post(Engine::kPerfOff);
}

void App::goHome() {
    mixer_ = options_ = edit_ = config_ = file_ = fwPage_ = inPage_ = midiPage_ = false;
    midi_.learn(-1);
    fxPage_ = -1;
    fxBar_ = modeBar_ = false;
    confirm_ = -1;
}

void App::onPad(int pad, bool down) {
    if (pad < 0 || pad >= ot::kPadCount) return;
    const int t = pad / ot::kPadCols, col = pad % ot::kPadCols;
    if (t >= kTracks) return;
    if (perf_ && col == 3) {   // performance: Freeze for a Clouds slot (row 1 FX 1, row 2 FX 2)
        if (down && t < kFxSlots && engine_.fxType(t) == FxType::kClouds) {
            const int on = engine_.fxKnob(t, kCloudsFreeze) ? 0 : 1;
            engine_.post(Engine::kFxKnob, uint8_t(t), int16_t((kCloudsFreeze << 8) | on));
        }
        return;
    }
    if (perf_ && col >= 4) {
        // Performance: held effects, or Shift + pad to latch / unlatch.
        const int fx = col - 4;
        const uint8_t bit = uint8_t(1u << fx);
        if (down) {
            selected_ = t;
            heldPads_[t] |= bit;
            heldFx_ = fx;   // the encoder now turns this effect's knob
            bool on = true;
            if (shift_) {
                perfLatch_[t] ^= bit;
                on = (perfLatch_[t] & bit) != 0;
            }
            engine_.post(Engine::kPerf, uint8_t(t), int16_t((fx << 8) | (on ? 1 : 0)));
        } else {
            heldPads_[t] &= uint8_t(~bit);
            if (!(perfLatch_[t] & bit)) engine_.post(Engine::kPerf, uint8_t(t), int16_t(fx << 8));
            // The knob goes to another effect pad still held, if any.
            heldFx_ = -1;
            for (int x = 0; x < kTracks && heldFx_ < 0; ++x)
                for (int e = 0; e < kPerfFxCount; ++e)
                    if ((heldPads_[x] >> e) & 1) {
                        heldFx_ = e;
                        break;
                    }
        }
        return;
    }
    if (!down) return;
    if (t != selected_) modeBar_ = false;   // the mode bar belongs to the selected track
    selected_ = t;
    switch (col) {
        case 0: record(t); break;
        case 1: engine_.post(Engine::kPlayStop, uint8_t(t)); break;
        case 2: engine_.post(Engine::kMute, uint8_t(t)); break;
        case 3: engine_.post(Engine::kClear, uint8_t(t)); break;
        default: engine_.post(Engine::kJump, uint8_t(t), int16_t(col - 4)); break;
    }
}

// ---------------------------------------------------------------------------
// Pad LEDs: each row mirrors its lane.

void App::updateLeds(uint32_t now) {
    // The pads are single-colour (IS31FL3731, 32 levels; the driver takes
    // max(r, g, b) as brightness), so state is brightness and blinking only.
    // Grey here makes the simulator look like the device.
    ot::ILeds& L = *p_.leds;
    const bool fast = (now / 125) & 1;                 // 4 Hz: recording
    const bool slow = (now / 300) & 1;                 // ~1.7 Hz: overdub
    constexpr float kOff = 0.0f, kFaint = 0.06f, kDimL = 0.18f, kMid = 0.45f, kFull = 1.0f;
    const uint32_t beat = engine_.barFrames() / (engine_.beatsPerBar() ? engine_.beatsPerBar() : 4);
    for (int t = 0; t < kTracks; ++t) {
        const TrackView& v = engine_.view(t);
        const TrackState s = TrackState(v.state.load());
        const bool muted = v.muted.load();
        const uint32_t len = v.len.load(), pos = v.pos.load();
        const bool content = len > 0;
        const int row = t * ot::kPadCols;
        auto set = [&](int col, float lv) {
            const uint8_t b = uint8_t(lv * 255.0f * float(cfg_.pads) / 100.0f);   // Config: pad brightness
            L.setPad(uint8_t(row + col), b, b, b);
        };

        // Col 1 REC: fast blink recording, slow blink overdub, a flash per beat
        // in the count-in, faint otherwise so it can be found.
        float rec = kFaint;
        if (s == TrackState::kRecording) rec = fast ? kFull : kDimL;
        else if (s == TrackState::kOverdubbing) rec = slow ? kFull : kMid;
        else if (s == TrackState::kCountIn) rec = beat && (v.startIn.load() % beat) > beat / 2 ? kFull : kFaint;
        else if (s == TrackState::kArmed) rec = slow ? kMid : kFaint;   // waiting for sound
        set(0, rec);
        // PLAY: lit while playing, blinking while waiting for its bar line.
        set(1, s == TrackState::kPlaying || s == TrackState::kOverdubbing ? kMid
               : s == TrackState::kQueued ? (fast ? kFull : kFaint) : (content ? kFaint : kOff));
        set(2, muted ? kFull : (content ? kFaint : kOff));
        if (perf_)   // col 4 is Freeze (rows 1 / 2) for a Clouds slot
            set(3, t < kFxSlots && engine_.fxType(t) == FxType::kClouds
                       ? (engine_.fxKnob(t, kCloudsFreeze) ? kFull : kDimL) : kOff);
        else
            set(3, content || s == TrackState::kRecording ? kFaint : kOff);
        if (perf_) {
            // Performance: the eight effect pads, full while their effect runs.
            const uint8_t on = v.perf.load();
            for (int c = 0; c < kPerfFxCount; ++c) set(4 + c, !content ? kOff : ((on >> c) & 1) ? kFull : kDimL);
            continue;
        }
        // Cols 5..12: the playhead's eighth.
        const int seg = content ? int(uint64_t(pos) * kSegments / len) : -1;
        for (int c = 0; c < kSegments; ++c) set(4 + c, !content ? kOff : (c == seg ? (muted ? kMid : kFull) : kFaint));
    }
    L.show();
}

// ---------------------------------------------------------------------------
// Waveform bitmaps (L8 intensity, tinted by COLOR_RGB at draw time)

void App::renderWave(int t, uint32_t span) {
    renderBitmap(t, span, kWaveW, kWaveH, kWaveRamG + uint32_t(t) * kWaveBytes);
}

void App::renderBitmap(int t, uint32_t span, int w, int h, uint32_t ramg) {
    // The waveform of `span` loop frames as a w x h L8 (luminance) bitmap
    // (tinted at draw time), sent to RAM_G in bands of rows that fit the
    // staging buffer.
    const TrackMemory& m = engine_.memory(t);
    const TrackView& v = engine_.view(t);
    const uint32_t start = v.start.load();   // loop frame 0 in the buffer (trim)
    const uint32_t written = v.written.load();
    const bool mono = v.mono.load();
    const int mid = h / 2;
    const uint32_t cellEnd = (start + written + kPeakFrames - 1) / kPeakFrames;
    // Half-heights per column, left and right (mono: the same).
    static uint8_t hl[1024], hr[1024];
    bool content[1024];
    for (int x = 0; x < w && x < 1024; ++x) {
        const uint32_t c0 = uint32_t((uint64_t(start) + uint64_t(span) * x / uint32_t(w)) / kPeakFrames);
        uint32_t c1 = uint32_t((uint64_t(start) + uint64_t(span) * (x + 1) / uint32_t(w)) / kPeakFrames);
        if (c1 <= c0) c1 = c0 + 1;
        uint8_t pl = 0, pr = 0;
        for (uint32_t c = c0; c < c1 && c < cellEnd; ++c) {
            const uint8_t a = mono ? m.peaks[c] : m.peaks[2 * c], bb = mono ? a : m.peaks[2 * c + 1];
            if (a > pl) pl = a;
            if (bb > pr) pr = bb;
        }
        content[x] = c0 < cellEnd;
        hl[x] = uint8_t(pl ? 1 + pl * (mid - 2) / 255 : 0);
        hr[x] = uint8_t(pr ? 1 + pr * (mid - 2) / 255 : 0);
    }
    // Intensity per distance from the centre line: bright core, darker edges.
    // L8 (one byte of luminance), so the greys stay neutral when tinted.
    auto shade = [&](int d) { return uint8_t(255 - d * 150 / (mid ? mid : 1)); };
    const uint8_t baseline = 0x2C;
    // Stereo: the left channel grows up from the centre line, the right one down.
    const int bandRows = int(p_.scratchBytes / uint32_t(w));
    for (int y0 = 0; y0 < h; y0 += bandRows) {
        const int y1 = y0 + bandRows < h ? y0 + bandRows : h;
        uint8_t* px = p_.scratch;
        std::memset(px, 0, size_t(y1 - y0) * size_t(w));
        for (int y = y0; y < y1; ++y) {
            uint8_t* row = px + size_t(y - y0) * size_t(w);
            const int d = y < mid ? mid - y : y - mid;
            const uint8_t s = shade(d);
            for (int x = 0; x < w; ++x) {
                if (y == mid) row[x] = baseline;
                else if (content[x] && ((y < mid && d <= hl[x]) || (y > mid && d <= hr[x]))) row[x] = s;
            }
        }
        d_->writeRamG(ramg + uint32_t(y0) * uint32_t(w), px, uint32_t(y1 - y0) * uint32_t(w));
        uploadBytes_ += uint32_t(y1 - y0) * uint32_t(w);
    }
    ++waveUploads_;
}

void App::updateWaves(uint32_t now) {
    // Each upload is a blocking SPI write (58 KB, ~16 ms at 30 MHz), so at most
    // one lane per frame, the lanes taking turns: several lanes recording at
    // once cannot stall the main loop (pads, LEDs) for longer than one upload.
    const uint32_t bar = engine_.barFrames();
    for (int i = 0; i < kTracks; ++i) {
        const int t = (waveNext_ + i) % kTracks;
        const TrackView& v = engine_.view(t);
        const TrackState s = TrackState(v.state.load());
        if (s == TrackState::kEmpty || s == TrackState::kClearing || s == TrackState::kCountIn ||
            s == TrackState::kArmed) {
            waveValid_[t] = false;
            continue;
        }
        uint32_t span = v.len.load();
        if (s == TrackState::kRecording) {
            // A known end (fixed length, pending stop) sets the view; otherwise
            // whole bars, growing a bar at a time.
            const uint32_t w = v.written.load(), stopIn = v.stopIn.load();
            if (stopIn) span = w + stopIn;
            else {
                span = bar ? bar : kSampleRate * 2;
                while (span < w) span += bar ? bar : span;
            }
            if (waveValid_[t] && now - waveDrawnMs_[t] < kRecWaveMs) continue;
        }
        const uint32_t ver = v.peakVersion.load();
        if (waveValid_[t] && ver == waveVersion_[t] && span == waveSpan_[t]) continue;
        if (s == TrackState::kOverdubbing && waveValid_[t] && now - waveDrawnMs_[t] < kRecWaveMs) continue;
        renderWave(t, span);
        waveVersion_[t] = ver;
        waveSpan_[t] = span;
        waveDrawnMs_[t] = now;
        waveValid_[t] = true;
        waveNext_ = (t + 1) % kTracks;
        return;
    }
}

// ---------------------------------------------------------------------------
// Drawing helpers

void App::color(uint32_t rgb) {
    // Greyscale: every colour as its luminance (Rec. 601 weights).
    const uint8_t l = uint8_t((77u * r8(rgb) + 150u * g8(rgb) + 29u * b8(rgb)) >> 8);
    d_->colorRGB(l, l, l);
    ++words_;
}

void App::panel(int x0, int y0, int x1, int y1, int radius, uint32_t fill) {
    // Shadow down and to the right, then a lit rim along the top, then the face.
    color(0x000000);
    alpha(150);
    fillRect(x0 + 1, y0 + 3, x1 + 3, y1 + 5, radius + 2);
    alpha(255);
    color(lighten(fill, 0x16));
    fillRect(x0, y0, x1, y1 - 1, radius);
    color(fill);
    fillRect(x0, y0 + 1, x1, y1, radius);
}

void App::well(int x0, int y0, int x1, int y1, int radius, uint32_t fill) {
    // A recess: its top edge in shadow (two rects: pages like the options
    // page draw dozens of these within the display-list budget).
    color(0x000000);
    fillRect(x0, y0 - 1, x1, y1, radius);
    color(fill);
    fillRect(x0, y0 + 1, x1, y1, radius);
}

void App::alpha(uint8_t a) {
    d_->colorA(a);
    ++words_;
}

void App::fillRect(int x0, int y0, int x1, int y1, int radius) {
    // FT81x RECTS with a line width draw rounded corners; the corners grow outwards,
    // so the vertices move in by the radius.
    d_->lineWidth(uint16_t(radius > 0 ? radius * 16 : 16));
    d_->begin(Disp::kRects);
    d_->vertex2f((x0 + radius) * 16, (y0 + radius) * 16);
    d_->vertex2f((x1 - radius) * 16, (y1 - radius) * 16);
    d_->end();
    words_ += 5;
}

void App::hline(int x0, int x1, int y, int width16) {
    d_->lineWidth(uint16_t(width16));
    d_->begin(Disp::kLines);
    d_->vertex2f(x0 * 16, y * 16);
    d_->vertex2f(x1 * 16, y * 16);
    d_->end();
    words_ += 5;
}

void App::vline(int x, int y0, int y1, int width16) {
    d_->lineWidth(uint16_t(width16));
    d_->begin(Disp::kLines);
    d_->vertex2f(x * 16, y0 * 16);
    d_->vertex2f(x * 16, y1 * 16);
    d_->end();
    words_ += 5;
}

void App::dot(int x, int y, int radius) {
    d_->pointSize(uint16_t(radius * 16));
    d_->begin(Disp::kPoints);
    d_->vertex2f(x * 16, y * 16);
    d_->end();
    words_ += 4;
}

void App::text(int x, int y, uint8_t font, uint16_t opt, const char* s) {
    // CMD_TEXT places glyphs with VERTEX2II, which only reaches x = 0..511: for
    // text further right, move the origin with VERTEX_TRANSLATE_X and keep the
    // local x where the glyphs fit for each alignment.
    int local = x;
    if (x >= 400) local = (opt & Disp::kOptRightX) ? 500 : ((opt & Disp::kOptCenterX) ? 256 : 8);
    const int shift = x - local;
    if (shift) d_->vertexTranslateX(shift * 16);
    d_->text(int16_t(x - shift), int16_t(y), font, opt, s);
    if (shift) d_->vertexTranslateX(0);
    words_ += uint32_t(std::strlen(s)) + 4 + (shift ? 2 : 0);
}

void App::scissor(int x, int y, int w, int h) {
    d_->scissorXY(uint16_t(x < 0 ? 0 : x), uint16_t(y < 0 ? 0 : y));
    d_->scissorSize(uint16_t(w), uint16_t(h));
    words_ += 2;
}

void App::scissorOff() { scissor(0, 0, 800, 480); }

// ---------------------------------------------------------------------------
// Screen

void App::render(uint32_t now) {
    words_ = 0;
    d_->dlStart();
    d_->clearColorRGB(r8(kBg), g8(kBg), b8(kBg));
    d_->clear(true, true, true);
    d_->gradient(0, 0, 0x202020, 0, 479, 0x080808);   // light falls from the top
    words_ += 8;

    if (fxPage_ >= 0) drawFxPage(now);
    else if (edit_) drawEditPage(now);
    else if (config_) drawConfig();
    else if (file_) drawFile();
    else if (inPage_) drawInput();
    else if (mixer_) drawMixer();
    else if (options_) drawOptions();
    else
        for (int t = 0; t < kTracks; ++t) drawLane(t, now);
    drawFKeys();
    // Pop-up knobs: F7 held (input gain), an effect pad held in performance mode.
    if (grainPanel()) {
        drawGrainPanel();
    } else if (f7Held_) {
        char v[12], title[32];
        const uint8_t vol = engine_.volume(selected_);
        std::snprintf(v, sizeof v, "%u", unsigned(vol));
        std::snprintf(title, sizeof title, "Track %d volume", selected_ + 1);
        drawPopupKnob(title, v, float(vol) / 100.0f, volShown_, kTrackColor[selected_], trackMeter_[selected_]);
    } else if (f6Held_) {
        char v[12];
        std::snprintf(v, sizeof v, "%u", unsigned(cfg_.gain));
        drawPopupKnob("Input gain", v, float(cfg_.gain) / 100.0f, gainShown_, 0x3DDC97,
                      inMeter_[0] > inMeter_[1] ? inMeter_[0] : inMeter_[1]);
    } else if (f4Held_ && shift_) {
        char v[12];
        const uint8_t c = engine_.opt(OptParam::kClick);
        std::snprintf(v, sizeof v, "%u", unsigned(c));
        drawPopupKnob("Click level", v, float(c) / 100.0f, clickShown_, kAmber,
                      engine_.opt(OptParam::kMetronome) ? 0.4f : 0.0f);
    } else if (f4Held_) {
        // Tempo: with a loop the knob is the tape speed (50 .. 200 %, 100 % at the top).
        char v[16];
        int lo, hi;
        tempoRange(lo, hi);
        const int bpm = engine_.opt(OptParam::kTempo);
        std::snprintf(v, sizeof v, "%d bpm", bpm);
        const bool loop = engine_.masterLen() != 0;
        const float frac = loop ? (engine_.speed() <= 1.0f ? (engine_.speed() - 0.5f) : 0.5f + (engine_.speed() - 1.0f) / 2.0f)
                                : float(bpm - lo) / float(hi - lo);
        char title[24];
        if (loop) std::snprintf(title, sizeof title, "Tempo  %d%%", int(engine_.speed() * 100.0f + 0.5f));
        else std::snprintf(title, sizeof title, "Tempo");
        drawPopupKnob(title, v, frac, tempoShown_, kAmber, 0.0f);
    } else if (perf_ && heldFx_ >= 0) {
        drawPerfKnob();
    }
    if (stats_) drawStats();
    d_->swap();
    ++words_;

    lastWords_ = words_;
    ++framesInWindow_;
    if (now - fpsWindowMs_ >= 1000) {
        fps_ = framesInWindow_ * 1000 / (now - fpsWindowMs_ ? now - fpsWindowMs_ : 1);
        framesInWindow_ = 0;
        fpsWindowMs_ = now;
        lastUploadBytes_ = uploadBytes_;
        lastWaveUploads_ = waveUploads_;
        waveUploads_ = 0;
        uploadBytes_ = 0;
    }
    // Meter ballistics: instant attack, ~300 ms release at 60 fps.
    for (int c = 0; c < 2; ++c) {
        const float in = engine_.inputLevel(c) / 255.0f;
        inMeter_[c] = in > inMeter_[c] ? in : inMeter_[c] * 0.9f;
        const float out = engine_.outputLevel(c) / 255.0f;
        outMeter_[c] = out > outMeter_[c] ? out : outMeter_[c] * 0.9f;
    }
    for (int s = 0; s < kFxSlots; ++s) {
        const float r = engine_.returnLevel(s) / 255.0f;
        retMeter_[s] = r > retMeter_[s] ? r : retMeter_[s] * 0.9f;
    }
    for (int t = 0; t < kTracks; ++t) {
        const float l = engine_.view(t).level.load() / 255.0f;
        trackMeter_[t] = l > trackMeter_[t] ? l : trackMeter_[t] * 0.9f;
    }
}

void App::drawLane(int t, uint32_t now) {
    const TrackView& v = engine_.view(t);
    const TrackState s = TrackState(v.state.load());
    const PlayMode mode = PlayMode(v.mode.load());
    const bool muted = v.muted.load();
    const uint32_t len = v.len.load(), pos = v.pos.load(), written = v.written.load();
    const uint32_t tc = kTrackColor[t];
    const bool sel = t == selected_;
    const bool empty = s == TrackState::kEmpty || s == TrackState::kClearing;
    const int y0 = kLaneY0 + t * (kLaneH + kLaneGap), y1 = y0 + kLaneH;
    char buf[48];

    // Card, outlined in the track colour when selected
    if (sel) {
        color(tc);
        alpha(110);
        fillRect(kLaneX0 - 2, y0 - 2, kLaneX1 + 2, y1 + 2, 10);
        alpha(255);
    }
    panel(kLaneX0, y0, kLaneX1, y1, 9, sel ? kLaneSel : kLane);

    // Waveform inset
    const int wx = kWaveX, wy = y0 + kWaveDy, wx1 = wx + kWaveW, wy1 = wy + kWaveH;
    well(wx - 2, wy - 2, wx1 + 2, wy1 + 2, 6, 0x050505);
    if (s == TrackState::kRecording || s == TrackState::kCountIn || s == TrackState::kArmed) {
        color(s == TrackState::kRecording ? kRed : kAmber);
        alpha(uint8_t(80 + 150 * pulse(now, 700)));
        d_->lineWidth(24);
        d_->begin(Disp::kLineStrip);
        d_->vertex2f((wx - 3) * 16, (wy - 3) * 16);
        d_->vertex2f((wx1 + 3) * 16, (wy - 3) * 16);
        d_->vertex2f((wx1 + 3) * 16, (wy1 + 3) * 16);
        d_->vertex2f((wx - 3) * 16, (wy1 + 3) * 16);
        d_->vertex2f((wx - 3) * 16, (wy - 3) * 16);
        d_->end();
        words_ += 8;
        alpha(255);
    }

    if (s == TrackState::kArmed) {
        // Waiting for sound: the input level against the threshold, in dB.
        auto pos = [](float db) { return db < -60.0f ? 0.0f : (db > 0.0f ? 1.0f : (db + 60.0f) / 60.0f); };
        const float in = inMeter_[0] > inMeter_[1] ? inMeter_[0] : inMeter_[1];
        const float lv = pos(in > 0.0001f ? 20.0f * std::log10(in) : -100.0f);
        const float th = pos(thresholdDb(engine_.opt(OptParam::kThreshold)));
        const int mx0 = wx + 60, mx1 = wx1 - 60, my = wy + kWaveH / 2 + 14;
        color(kAmber);
        text(wx + kWaveW / 2, wy + 22, kFontSmall, Disp::kOptCenter, "Waiting for sound");
        color(kFaint);
        fillRect(mx0, my - 5, mx1, my + 5, 5);
        if (lv > 0.0f) {
            color(lv >= th ? kRed : 0x3DDC97);
            fillRect(mx0, my - 5, mx0 + 10 + int((mx1 - mx0 - 10) * lv), my + 5, 5);
        }
        const int tx = mx0 + int((mx1 - mx0) * th);
        color(kInk);
        vline(tx, my - 11, my + 11, 32);
        std::snprintf(buf, sizeof buf, "%d dB", int(std::lround(thresholdDb(engine_.opt(OptParam::kThreshold)))));
        color(kDim);
        text(tx, my + 24, kFontSmall, Disp::kOptCenter, buf);
    } else if (s == TrackState::kCountIn) {
        // Beats left until the take starts, counting down on the beat.
        const uint32_t beat = engine_.barFrames() / (engine_.beatsPerBar() ? engine_.beatsPerBar() : 4);
        const uint32_t left = beat ? (v.startIn.load() + beat - 1) / beat : 0;
        std::snprintf(buf, sizeof buf, "Recording in %lu", static_cast<unsigned long>(left));
        color(kAmber);
        text(wx + kWaveW / 2, wy + kWaveH / 2, kFontBig, Disp::kOptCenter, buf);
    } else if (empty) {
        color(kFaint);
        hline(wx + 8, wx1 - 8, wy + kWaveH / 2, 16);
        color(kDim);
        text(wx + kWaveW / 2, wy + kWaveH / 2, kFontSmall, Disp::kOptCenter,
             s == TrackState::kClearing ? "clearing..." : "pad 1 or REC to record");
    } else if (waveValid_[t]) {
        // Played part bright, the rest dimmed: the same bitmap twice under two
        // scissors. Reverse reads right to left, so its "played" side is the right.
        const uint32_t span = waveSpan_[t] ? waveSpan_[t] : 1;
        int px = wx + int(uint64_t(kWaveW) * (pos % span) / span);
        if (s == TrackState::kRecording) px = wx + int(uint64_t(kWaveW) * written / span);
        const uint32_t ramg = kWaveRamG + uint32_t(t) * kWaveBytes;
        const float dim = muted || s == TrackState::kStopped || s == TrackState::kQueued ? 0.35f : 1.0f;
        const bool rev = mode == PlayMode::kReverse && s != TrackState::kRecording;
        const float leftK = rev ? 0.45f * dim : dim, rightK = rev ? dim : 0.45f * dim;
        if (px > wx) {
            scissor(wx, wy, px - wx, kWaveH);
            color(scale(tc, leftK));
            d_->drawL8(int16_t(wx), int16_t(wy), ramg, kWaveW, kWaveH, 1);
            words_ += 13;
        }
        if (px < wx1) {
            scissor(px, wy, wx1 - px, kWaveH);
            color(scale(tc, rightK));
            d_->drawL8(int16_t(wx), int16_t(wy), ramg, kWaveW, kWaveH, 1);
            words_ += 13;
        }
        scissorOff();

        if (!perf_) drawGrid(t, wx, wy, span);   // the effect cells take its place (display-list budget)
        color(kDim);
        if (v.mono.load()) {
            text(wx1 - 6, wy + 2, kFontSmall, Disp::kOptRightX, "M");
        } else {
            text(wx1 - 6, wy + 2, kFontSmall, Disp::kOptRightX, "L");
            text(wx1 - 6, wy1 - 16, kFontSmall, Disp::kOptRightX, "R");
        }
        if (s == TrackState::kQueued) {   // waiting for its bar line
            color(kAmber);
            text(wx1 - 24, wy + 2, kFontSmall, Disp::kOptRightX, "starts on the next bar");
        }
        // A pending stop: how many beats are left in the take.
        const uint32_t stopIn = v.stopIn.load();
        if (s == TrackState::kRecording && stopIn) {
            const uint32_t beat = engine_.barFrames() / (engine_.beatsPerBar() ? engine_.beatsPerBar() : 4);
            const uint32_t left = beat ? (stopIn + beat - 1) / beat : 0;
            std::snprintf(buf, sizeof buf, "ends in %lu beat%s", static_cast<unsigned long>(left), left == 1 ? "" : "s");
            color(kRed);
            text(wx1 - 24, wy + 2, kFontSmall, Disp::kOptRightX, buf);
        }

        // Playhead with a glow
        const uint32_t head = s == TrackState::kRecording ? kRed : (s == TrackState::kOverdubbing ? kAmber : kInk);
        color(head);
        alpha(60);
        vline(px, wy - 1, wy1 + 1, 96);
        alpha(255);
        vline(px, wy - 1, wy1 + 1, 24);
    }

    if (perf_ && v.len.load()) drawPerfCells(t, wx, wy);   // effects need a loop

    // Corner tag: the track number, then what is not the default (length,
    // mode, mute). The selected track's full state is on the F-keys.
    {
        char tag[48];
        int n = 0;
        if (len) {
            fmtSeconds(tag, sizeof tag, len);
            n = int(std::strlen(tag));
        } else if (s == TrackState::kRecording) {
            fmtSeconds(tag, sizeof tag, written);
            n = int(std::strlen(tag));
        }
        if (mode != PlayMode::kForward && n < int(sizeof tag) - 2)
            n += std::snprintf(tag + n, sizeof tag - size_t(n), "%s%s", n ? "  " : "", modeInfo(mode).name);
        if (muted && n < int(sizeof tag) - 2) std::snprintf(tag + n, sizeof tag - size_t(n), "%sMuted", n ? "  " : "");
        else if (!n) tag[0] = '\0';
        const int tx = wx + 4, ty = wy + 3;
        color(0x05070A);
        alpha(200);
        fillRect(tx - 2, ty - 1, tx + 20 + (tag[0] ? 8 * int(std::strlen(tag)) : 0), ty + 17, 4);
        alpha(255);
        color(empty ? scale(tc, 0.5f) : tc);
        std::snprintf(buf, sizeof buf, "%d", t + 1);
        text(tx + 4, ty, kFontSmall, 0, buf);
        if (tag[0]) {
            color(muted ? 0xC0A000 : kDim);
            text(tx + 20, ty, kFontSmall, 0, tag);
        }
    }

    // Hint over the waveform
    if (hintTrack_ == t && hintText_ && int32_t(hintUntilMs_ - now) > 0) {
        color(0x262D38);
        alpha(235);
        fillRect(wx + 110, wy + kWaveH / 2 - 16, wx1 - 110, wy + kWaveH / 2 + 16, 8);
        alpha(255);
        color(kAmber);
        text(wx + kWaveW / 2, wy + kWaveH / 2, kFontSmall, Disp::kOptCenter, hintText_);
    }
}

void App::drawFKeys() {
    // One cell per screen key, centred over it (key i spans x = 100 i .. 100 i + 99).
    const uint32_t tc = kTrackColor[selected_];
    const TrackView& v = engine_.view(selected_);
    const TrackState s = TrackState(v.state.load());
    const PlayMode mode = engine_.mode(selected_);
    char val[8][16];
    const char* title[8];
    const uint32_t now = p_.clock->millis();
    for (int i = 0; i < 8; ++i) {
        const int x0 = i * kKeyW + 3, x1 = (i + 1) * kKeyW - 3, cx = i * kKeyW + kKeyW / 2;
        bool hot = false;
        uint32_t hotCol = 0;   // a lit key's own colour (0: the page's)
        if (modeBar_) {
            const int m = kModeBarKeys[modePage_ & 1][i];
            title[i] = "";
            std::snprintf(val[i], sizeof val[i], "%s",
                          m >= 0 ? kModes[m].name : (m == kModeMore ? (modePage_ ? "< Back" : "More >") : ""));
            hot = m >= 0 && int(mode) == m;
        } else if (shift_ && !mixer_ && !options_ && fxPage_ < 0 && !edit_ && !config_ && !file_ && !inPage_ &&
                   !f5Held_ && !f6Held_ && !f7Held_) {
            // The Shift layer of the home page.
            const uint8_t u = v.dubUndo.load();
            const bool loop = v.len.load() != 0;
            const bool dubbable = loop && modeInfo(mode).canDub;
            switch (i) {
                case 0:
                    title[i] = "REPLACE";
                    std::snprintf(val[i], sizeof val[i], "%s",
                                  s == TrackState::kOverdubbing ? "Stop" : (dubbable ? "Punch in" : "-"));
                    break;
                case 1:
                    title[i] = "ALL";
                    std::snprintf(val[i], sizeof val[i], "%s", engine_.running() ? "Stop all" : "Play all");
                    break;
                case 2:
                    title[i] = "SOLO";
                    std::snprintf(val[i], sizeof val[i], "%s", solo_ == selected_ ? "Off" : "Track");
                    hot = solo_ == selected_;
                    break;
                case 3:
                    title[i] = "CLICK";   // hold + encoder
                    std::snprintf(val[i], sizeof val[i], "%u", unsigned(engine_.opt(OptParam::kClick)));
                    break;
                case 4:
                    title[i] = "UNDO";
                    std::snprintf(val[i], sizeof val[i], "%s", u == 1 ? "Overdub" : "-");
                    break;
                case 5:
                    title[i] = "REDO";
                    std::snprintf(val[i], sizeof val[i], "%s", u == 2 ? "Overdub" : "-");
                    break;
                case 6:
                    title[i] = "DOUBLE";
                    std::snprintf(val[i], sizeof val[i], "%s", loop ? "x2" : "-");
                    break;
                default:
                    title[i] = "HALVE";
                    std::snprintf(val[i], sizeof val[i], "%s", loop ? "1/2" : "-");
                    break;
            }
        } else if (grainPanel() && i < kGrainKnobs) {   // F5 held: F1..F4 pick a grain knob
            title[i] = kGrainKnobInfo[i].name;
            grainText(i, val[i], sizeof val[i]);
            hot = grainSel_ == i;
        } else if (fxPage_ >= 0 && fxBar_) {   // the effect types: the key under one picks it
            title[i] = "";
            std::snprintf(val[i], sizeof val[i], "%s", i < kFxTypeCount ? kFxTypes[i].name : "");
            hot = int(engine_.fxType(fxPage_)) == i;
        } else if (fxPage_ >= 0) {   // F1 type, F2..F5 knobs, F6 return, F7 other slot, F8 home
            const FxTypeInfo& ti = fxTypeInfo(engine_.fxType(fxPage_));
            if (i == 0) {
                title[i] = "TYPE";
                std::snprintf(val[i], sizeof val[i], "%s", ti.name);
            } else if (i <= kFxBank) {
                const int kn = fxBankOf(fxPage_) * kFxBank + i - 1;
                title[i] = ti.knob[kn].name ? ti.knob[kn].name : "";
                if (ti.knob[kn].name) fxKnobText(fxPage_, kn, val[i], sizeof val[i]);
                else val[i][0] = 0;
                hot = fxSel_ == kn;
            } else if (i == 5) {
                title[i] = "RETURN";
                std::snprintf(val[i], sizeof val[i], "%u",
                              unsigned(engine_.mix(fxPage_ ? MixParam::kReturn2 : MixParam::kReturn1)));
                hot = fxSel_ == kFxKnobs;
            } else if (i == 6) {
                title[i] = "SLOT";
                std::snprintf(val[i], sizeof val[i], "FX %d", fxPage_ ? 1 : 2);
            } else {
                title[i] = "HOME";
                std::snprintf(val[i], sizeof val[i], "Loops");
            }
        } else if (config_ && fwPage_) {   // F1 flash (twice), F2 rescan, F7 back, F8 home
            const bool armed = confirm_ == 0 && int32_t(confirmUntilMs_ - p_.clock->millis()) > 0;
            static const char* const kT[8] = {"FLASH", "RESCAN", "", "", "", "", "BACK", "HOME"};
            title[i] = kT[i];
            const char* v = "";
            if (i == 0) v = !fwCount_ ? "-" : (armed ? "Sure?" : "Image");
            else if (i == 1) v = "Card";
            else if (i == 6) v = "Config";
            else if (i == 7) v = "Loops";
            std::snprintf(val[i], sizeof val[i], "%s", v);
            hot = i == 0 && armed;
        } else if (inPage_) {   // quick keys for the main switches, F6 / F8 close
            const uint8_t ch = engine_.opt(OptParam::kInput, selected_);
            static const char* const kCh[4] = {"Stereo", "Mono L", "Mono R", "Mono sum"};
            switch (i) {
                case 0: title[i] = "SOURCE"; std::snprintf(val[i], sizeof val[i], "%s", sourceName(cfg_.source)); break;
                case 1: title[i] = "MONITOR"; std::snprintf(val[i], sizeof val[i], "%s", engine_.monitoring() ? "On" : "Off"); break;
                case 2: title[i] = "CHANNELS"; std::snprintf(val[i], sizeof val[i], "%s", kCh[ch & 3]); break;
                case 3: title[i] = "METRONOME"; std::snprintf(val[i], sizeof val[i], "%s", engine_.opt(OptParam::kMetronome) ? "On" : "Off"); break;
                case 4: title[i] = "FEEDBACK"; std::snprintf(val[i], sizeof val[i], "%u%%", unsigned(engine_.feedback())); break;
                case 5: title[i] = "INPUT"; std::snprintf(val[i], sizeof val[i], "Close"); break;
                case 7: title[i] = "HOME"; std::snprintf(val[i], sizeof val[i], "Loops"); break;
                default: title[i] = ""; val[i][0] = '\0'; break;
            }
        } else if (config_ && midiPage_) {   // F1 learn, F2 clear, F7 back, F8 home
            const bool act = midiRow_ > 0;
            static const char* const kT[8] = {"LEARN", "CLEAR", "", "", "", "", "BACK", "HOME"};
            title[i] = kT[i];
            const char* v = "";
            if (i == 0) v = !act ? "-" : (midi_.learning() == midiRow_ - 1 ? "Waiting" : "Action");
            else if (i == 1) v = act ? "Binding" : "-";
            else if (i == 6) v = "Config";
            else if (i == 7) v = "Loops";
            std::snprintf(val[i], sizeof val[i], "%s", v);
            hot = i == 0 && act && midi_.learning() == midiRow_ - 1;
        } else if (config_) {   // F1 firmware, F2 MIDI, F8 home: Up / Down pick a setting, the encoder sets it
            title[i] = i == 7 ? "HOME" : (i == 0 ? "FIRMWARE" : (i == 1 ? "MIDI" : ""));
            std::snprintf(val[i], sizeof val[i], "%s", i == 7 ? "Loops" : (i == 0 ? "Update" : (i == 1 ? "Clock, map" : "")));
        } else if (file_) {   // projects and export
            const bool armed = confirm_ == i && int32_t(confirmUntilMs_ - p_.clock->millis()) > 0;
            switch (i) {
                case 0:
                    title[i] = "SAVE";
                    if (files_.current()) std::snprintf(val[i], sizeof val[i], "Proj %03d", files_.current());
                    else std::snprintf(val[i], sizeof val[i], "New");
                    break;
                case 1: title[i] = "SAVE NEW"; std::snprintf(val[i], sizeof val[i], "Project"); break;
                case 2:
                    title[i] = "LOAD";
                    std::snprintf(val[i], sizeof val[i], "%s", armed ? "Sure?" : "Selected");
                    hot = armed;
                    break;
                case 4:
                    title[i] = "EXPORT";
                    std::snprintf(val[i], sizeof val[i], "Track %c", char('1' + selected_));
                    break;
                case 5: title[i] = "EXPORT"; std::snprintf(val[i], sizeof val[i], "All"); break;
                case 7: title[i] = "HOME"; std::snprintf(val[i], sizeof val[i], "Loops"); break;
                default: title[i] = ""; val[i][0] = 0; break;
            }
        } else if (edit_) {   // the loop editor
            static const char* const kSnap[4] = {"Bar", "Beat", "1/16", "Free"};
            const TrackView& ev = engine_.view(editTrack_);
            const bool armed = confirm_ == i && int32_t(confirmUntilMs_ - p_.clock->millis()) > 0;
            switch (i) {
                case 0: title[i] = "START"; std::snprintf(val[i], sizeof val[i], "Marker"); hot = editMarker_ == 0; break;
                case 1: title[i] = "END"; std::snprintf(val[i], sizeof val[i], "Marker"); hot = editMarker_ == 1; break;
                case 2: title[i] = "SNAP"; std::snprintf(val[i], sizeof val[i], "%s", kSnap[editSnap_ & 3]); break;
                case 3: title[i] = "TRIM"; std::snprintf(val[i], sizeof val[i], "Keep sel"); break;
                case 4: title[i] = "CUT"; std::snprintf(val[i], sizeof val[i], "%s", armed ? "Sure?" : "Remove"); hot = armed; break;
                case 5: title[i] = "ERASE"; std::snprintf(val[i], sizeof val[i], "%s", armed ? "Sure?" : "Silence"); hot = armed; break;
                case 6:
                    title[i] = "UNDO";
                    std::snprintf(val[i], sizeof val[i], "%s", ev.canUndo.load() ? "Trim" : "-");
                    break;
                default: title[i] = "HOME"; std::snprintf(val[i], sizeof val[i], "Loops"); break;
            }
        } else if (mixer_) {   // F1..F7 pick a strip, F8 home
            static const char* const kTitles[8] = {"TRACK", "TRACK", "TRACK", "TRACK", "FX 1", "FX 2", "MASTER", "HOME"};
            static const char* const kNames[8] = {"Track 1", "Track 2", "Track 3", "Track 4", "", "", "Level", "Loops"};
            title[i] = kTitles[i];
            std::snprintf(val[i], sizeof val[i], "%s",
                          i == 4 || i == 5 ? fxTypeInfo(engine_.fxType(i - 4)).name : kNames[i]);
            hot = strip_ == i;
        } else if (options_) {   // F1..F5 pick a column, F8 home
            static const char* const kTitles[8] = {"TRACK", "TRACK", "TRACK", "TRACK", "GLOBAL", "", "", "HOME"};
            static const char* const kNames[8] = {"Track 1", "Track 2", "Track 3", "Track 4", "Tempo", "", "", "Loops"};
            title[i] = kTitles[i];
            std::snprintf(val[i], sizeof val[i], "%s", kNames[i]);
            hot = optCol_ == i;
        } else {
            switch (i) {
                case 0:   // lit while it records: red, overdub amber, waiting (count-in, sound) dim amber
                    if (s == TrackState::kRecording) hotCol = scale(kRed, 0.6f + 0.4f * pulse(now, 700));
                    else if (s == TrackState::kOverdubbing)   // replace pulses like a take
                        hotCol = v.replace.load() ? scale(kRed, 0.6f + 0.4f * pulse(now, 700))
                                                  : scale(kAmber, 0.6f + 0.4f * pulse(now, 700));
                    else if (s == TrackState::kCountIn || s == TrackState::kArmed)
                        hotCol = scale(kAmber, 0.3f + 0.3f * pulse(now, 1200));
                    hot = hotCol != 0;
                    title[i] = "REC";
                    std::snprintf(val[i], sizeof val[i], "%s",
                                  s == TrackState::kEmpty ? "Record"
                                  : s == TrackState::kCountIn || s == TrackState::kArmed ? "Cancel"
                                  : s == TrackState::kRecording ? "Finish"
                                  : s == TrackState::kOverdubbing ? (v.replace.load() ? "Stop repl" : "Stop dub")
                                  : shift_ && (s == TrackState::kPlaying || s == TrackState::kStopped) &&
                                          modeInfo(mode).canDub ? "Replace"
                                  : s == TrackState::kClearing ? "-"
                                  : modeInfo(mode).canDub ? "Overdub" : "-");
                    break;
                case 1:   // lit while it plays; dim amber while it waits for its bar line
                    if (s == TrackState::kPlaying || s == TrackState::kOverdubbing) hotCol = scale(tc, 0.85f);
                    else if (s == TrackState::kQueued) hotCol = scale(kAmber, 0.3f + 0.3f * pulse(now, 1200));
                    hot = hotCol != 0;
                    title[i] = "PLAY";
                    std::snprintf(val[i], sizeof val[i], "%s",
                                  s == TrackState::kPlaying || s == TrackState::kOverdubbing ? "Stop"
                                  : s == TrackState::kStopped ? "Play"
                                  : s == TrackState::kQueued ? "Cancel" : "-");
                    break;
                case 2:
                    hot = v.muted.load();
                    hotCol = 0xC0A000;
                    title[i] = "MUTE";
                    std::snprintf(val[i], sizeof val[i], "%s", v.muted.load() ? "On" : "Off");
                    break;
                case 3:
                    title[i] = "TEMPO";   // amber while the metronome is on
                    std::snprintf(val[i], sizeof val[i], "%u bpm", unsigned(engine_.opt(OptParam::kTempo)));
                    break;
                case 4:
                    title[i] = "MODE";
                    std::snprintf(val[i], sizeof val[i], "%s", modeInfo(mode).name);
                    break;
                case 5:
                    title[i] = "INPUT";
                    std::snprintf(val[i], sizeof val[i], "%s", sourceName(cfg_.source));
                    break;
                case 6:   // the level and meter bar are drawn below
                    title[i] = "VOLUME";
                    std::snprintf(val[i], sizeof val[i], "%u", unsigned(engine_.volume(selected_)));
                    break;
                default:   // F8: free
                    title[i] = "";
                    val[i][0] = '\0';
                    break;
            }
        }
        uint32_t hc = tc;
        if (config_ || file_ || inPage_) hc = kAmber;
        else if (edit_) hc = (i == 4 || i == 5) ? kAmber : kTrackColor[editTrack_];
        else if (fxPage_ >= 0) hc = stripColor(4 + fxPage_);
        else if (mixer_ && !modeBar_) hc = stripColor(i);
        else if (options_ && !modeBar_) hc = i < kTracks ? kTrackColor[i] : (i == 4 ? kAmber : kDim);
        if (hot && hotCol) hc = hotCol;
        panel(x0, kKeyY0, x1, kKeyY1, 6, hot ? hc : kCell);
        if (title[i][0]) {
            uint32_t tcol = kDim;
            if (hot) tcol = kBg;
            else if (file_) tcol = i < 7 ? scale(hc, 0.9f) : kDim;
            else if (inPage_) tcol = i < 5 ? scale(hc, 0.9f) : kDim;
            else if (edit_) tcol = i < 7 ? scale(hc, 0.9f) : kDim;
            else if (fxPage_ >= 0) tcol = i < 6 ? scale(hc, 0.9f) : kDim;
            else if (mixer_) tcol = i < 7 ? scale(hc, 0.9f) : kDim;
            else if (options_) tcol = i < 5 ? scale(hc, 0.9f) : kDim;
            else if (!modeBar_ && i == 3) tcol = engine_.opt(OptParam::kMetronome) ? kAmber : kDim;   // F4 is global
            else if (!modeBar_ && (i < 5 || i == 6)) tcol = scale(tc, 0.9f);
            color(tcol);
            text(cx, kKeyY0 + 10, kFontSmall, Disp::kOptCenter, title[i]);
        }
        const bool bar = modeBar_ || (fxPage_ >= 0 && fxBar_);   // one-line labels
        color(hot ? kBg : kInk);
        text(cx, bar ? (kKeyY0 + kKeyY1) / 2 + 4 : kKeyY0 + 28, kFontSmall, Disp::kOptCenter, val[i]);
    }
    // Home page: the input level inside the INPUT cell, the track's level inside VOLUME
    if (!modeBar_ && !mixer_ && !options_ && fxPage_ < 0 && !edit_ && !config_ && !file_ && !inPage_) {
        const int x0 = 5 * kKeyW + 10, x1 = 6 * kKeyW - 10;
        for (int c = 0; c < 2; ++c) {
            const float m = inMeter_[c];
            if (m < 0.005f) continue;
            color(m > 0.9f ? kRed : (m > 0.6f ? kAmber : 0x3DDC97));
            hline(x0, x0 + int((x1 - x0) * m), kKeyY1 - 5 + c * 2, 16);
        }
        // The selected track's volume inside the VOLUME cell, its output level over it
        // (not under the Shift layer, where F7 is DOUBLE).
        const int vx0 = 6 * kKeyW + 10, vx1 = shift_ ? vx0 : 7 * kKeyW - 10, vy = kKeyY1 - 5;
        const uint8_t vol = engine_.volume(selected_);
        color(kFaint);
        hline(vx0, vx1, vy, 16);
        color(scale(tc, 0.45f));
        hline(vx0, vx0 + (vx1 - vx0) * vol / 100, vy, 16);
        if (trackMeter_[selected_] > 0.01f) {
            color(tc);
            hline(vx0, vx0 + int((vx1 - vx0) * trackMeter_[selected_] * vol / 100), vy, 16);
        }
    }
}

// ---------------------------------------------------------------------------
// Mixer page

bool App::mixParam(int strip, int row, MixParam& p, int& track, int& fxKnob) const {
    // A mixer cell is a mixer parameter, or (FX strips, top rows) one of the
    // slot's effect knobs: fxKnob >= 0, track = the slot.
    track = strip < kTracks ? strip : 0;
    fxKnob = -1;
    if (strip < kTracks) {
        p = row == 0 ? MixParam::kSend1 : (row == 1 ? MixParam::kSend2 : MixParam::kLevel);
        return true;
    }
    if (strip == 4 || strip == 5) {
        const int slot = strip - 4;
        track = slot;
        if (row == 2) {
            p = slot == 0 ? MixParam::kReturn1 : MixParam::kReturn2;
            return true;
        }
        if (!fxTypeInfo(engine_.fxType(slot)).knob[row].name) return false;
        fxKnob = row;
        return true;
    }
    if (strip == 6 && row == 2) {
        p = MixParam::kMaster;
        return true;
    }
    return false;
}

void App::mixerKey(uint8_t id) {
    MixParam p;
    int t, k;
    if (id == b::F8) {
        goHome();
        return;
    }
    if (b::isFKey(id)) strip_ = id - b::F1;
    else if (id == b::Left && strip_ > 0) --strip_;
    else if (id == b::Right && strip_ < kStrips - 1) ++strip_;
    else if (id == b::Up && row_ > 0 && mixParam(strip_, row_ - 1, p, t, k)) --row_;
    else if (id == b::Down && row_ < kRows - 1) ++row_;
    if (!mixParam(strip_, row_, p, t, k)) row_ = kRows - 1;   // master has only the fader
    if (strip_ < kTracks) selected_ = strip_;
}

void App::mixerEncoder(int delta) {
    MixParam p;
    int t, k;
    if (!mixParam(strip_, row_, p, t, k)) return;
    if (k >= 0) {
        stepFxKnob(t, k, delta);
        return;
    }
    int v = int(engine_.mix(p, t)) + delta * (shift_ ? 1 : 2);
    v = v < 0 ? 0 : (v > 100 ? 100 : v);
    engine_.post(Engine::kMix, uint8_t(t), int16_t((int(p) << 8) | v));
}

void App::stepFxKnob(int slot, int knob, int delta) {
    // List knobs (delay time) step one entry, and Clouds' pitch one semitone;
    // 0..100 knobs two (Shift: one).
    const FxKnob& kn = fxTypeInfo(engine_.fxType(slot)).knob[knob];
    if (!kn.name) return;
    const bool semis = engine_.fxType(slot) == FxType::kClouds && knob == 2;
    int v = int(engine_.fxKnob(slot, knob)) + delta * (kn.list || semis || shift_ ? 1 : 2);
    v = v < 0 ? 0 : (v > kn.max ? kn.max : v);
    engine_.post(Engine::kFxKnob, uint8_t(slot), int16_t((knob << 8) | v));
}

void App::fxKnobText(int slot, int knob, char* buf, size_t n) const {
    const FxKnob& kn = fxTypeInfo(engine_.fxType(slot)).knob[knob];
    const uint8_t v = engine_.fxKnob(slot, knob);
    if (!kn.name) std::snprintf(buf, n, "-");
    else if (kn.list) std::snprintf(buf, n, "%s", kn.list[v <= kn.max ? v : 0]);
    else if (engine_.fxType(slot) == FxType::kClouds && knob == 2)
        std::snprintf(buf, n, "%+d st", int(v) - kCloudsPitchZero);
    else std::snprintf(buf, n, "%u", unsigned(v));
}

float App::fxKnobFrac(int slot, int knob) const {
    const FxKnob& kn = fxTypeInfo(engine_.fxType(slot)).knob[knob];
    return kn.name && kn.max ? float(engine_.fxKnob(slot, knob)) / float(kn.max) : 0.0f;
}

void App::drawMixer() {
    for (int s = 0; s < kStrips; ++s) drawStrip(s);
    // The eighth column, over F8: how to drive the page.
    const int cx = 7 * kKeyW + kKeyW / 2;
    color(0x4A5563);
    text(cx, 200, kFontSmall, Disp::kOptCenter, "Up/Down");
    text(cx, 218, kFontSmall, Disp::kOptCenter, "row");
    text(cx, 248, kFontSmall, Disp::kOptCenter, "Encoder");
    text(cx, 266, kFontSmall, Disp::kOptCenter, "value");
    text(cx, 296, kFontSmall, Disp::kOptCenter, "Shift");
    text(cx, 314, kFontSmall, Disp::kOptCenter, "fine");
}

void App::drawStrip(int s) {
    static const char* const kStripName[kStrips] = {"Track 1", "Track 2", "Track 3", "Track 4", "FX 1", "FX 2",
                                                     "Master"};
    const int x0 = s * kKeyW + 4, x1 = (s + 1) * kKeyW - 4, cx = s * kKeyW + kKeyW / 2;
    const uint32_t sc = stripColor(s);
    const bool sel = strip_ == s;
    const bool emptyTrack = s < kTracks && TrackState(engine_.view(s).state.load()) == TrackState::kEmpty;
    char buf[16];

    panel(x0, 6, x1, 428, 8, sel ? kLaneSel : kLane);
    color(scale(sc, emptyTrack ? 0.35f : 0.85f));
    fillRect(x0 + 4, 10, x1 - 4, 36, 6);
    color(emptyTrack ? kDim : kBg);
    text(cx, 23, kFontSmall, Disp::kOptCenter, kStripName[s]);
    if (s == 4 || s == 5) {   // which effect is in the slot
        color(sc);
        text(cx, 41, kFontSmall, Disp::kOptCenter, fxTypeInfo(engine_.fxType(s - 4)).name);
    }

    auto outline = [&](int bx0, int by0, int bx1, int by1) {
        color(sc);
        fillRect(bx0 - 2, by0 - 2, bx1 + 2, by1 + 2, 8);
    };

    // Two small parameters: label, value, bar. Track sends use their slot's colour.
    for (int row = 0; row < 2; ++row) {
        MixParam p;
        int t, k;
        if (!mixParam(s, row, p, t, k)) continue;
        const int by0 = 52 + row * 64, by1 = by0 + 58;
        if (sel && row_ == row) outline(x0 + 4, by0, x1 - 4, by1);
        well(x0 + 4, by0, x1 - 4, by1, 6, kCell);
        const char* label;
        float fr;
        if (k >= 0) {
            label = fxTypeInfo(engine_.fxType(t)).knob[k].name;
            fxKnobText(t, k, buf, sizeof buf);
            fr = fxKnobFrac(t, k);
        } else {
            label = row == 0 ? "To FX 1" : "To FX 2";
            const uint8_t v = engine_.mix(p, t);
            std::snprintf(buf, sizeof buf, "%u", unsigned(v));
            fr = float(v) / 100.0f;
        }
        color(kDim);
        text(cx, by0 + 12, kFontSmall, Disp::kOptCenter, label);
        color(kInk);
        text(cx, by0 + 30, kFontSmall, Disp::kOptCenter, buf);
        const uint32_t bc = s < kTracks ? (row == 0 ? kDelayColor : kReverbColor) : sc;
        color(kFaint);
        fillRect(x0 + 12, by0 + 45, x1 - 12, by0 + 49, 2);
        color(scale(bc, emptyTrack ? 0.4f : 1.0f));
        fillRect(x0 + 12, by0 + 45, x0 + 16 + int((x1 - x0 - 28) * fr), by0 + 49, 2);
    }

    // The fader: level, return or master, with its meter beside it.
    MixParam p;
    int t, kk;
    if (!mixParam(s, 2, p, t, kk)) return;
    auto valueText = [&](uint8_t v) { std::snprintf(buf, sizeof buf, "%u", unsigned(v)); };
    auto frac = [](uint8_t v) { return float(v) / 100.0f; };
    const int fy0 = 184, fy1 = 424;
    if (sel && row_ == 2) outline(x0 + 4, fy0, x1 - 4, fy1);
    well(x0 + 4, fy0, x1 - 4, fy1, 6, kCell);
    color(kDim);
    text(cx, fy0 + 12, kFontSmall, Disp::kOptCenter, s < kTracks || s == 6 ? "Level" : "Return");
    const uint8_t v = engine_.mix(p, t);
    const int sy0 = fy0 + 30, sy1 = fy1 - 34;           // slot
    const int ky = sy1 - int((sy1 - sy0) * frac(v));     // knob
    color(kFaint);
    fillRect(cx - 4, sy0, cx + 4, sy1, 4);
    color(scale(sc, emptyTrack ? 0.4f : 1.0f));
    fillRect(cx - 4, ky, cx + 4, sy1, 4);
    color(kInk);
    fillRect(cx - 14, ky - 4, cx + 14, ky + 4, 3);
    // Meters: one for a track or a return, left and right for the master.
    auto meter = [&](int mx, float m) {
        color(0x1E252F);
        fillRect(mx - 2, sy0, mx + 2, sy1, 2);
        if (m < 0.005f) return;
        if (m > 1.0f) m = 1.0f;
        color(m > 0.9f ? kRed : (m > 0.6f ? kAmber : 0x3DDC97));
        fillRect(mx - 2, sy1 - int((sy1 - sy0) * m), mx + 2, sy1, 2);
    };
    if (s < kTracks) meter(x1 - 16, trackMeter_[s]);
    else if (s < 6) meter(x1 - 16, retMeter_[s - 4]);
    else {
        meter(x0 + 16, outMeter_[0]);
        meter(x1 - 16, outMeter_[1]);
    }
    valueText(v);
    color(kInk);
    text(cx, fy1 - 16, kFontSmall, Disp::kOptCenter, buf);
}

// ---------------------------------------------------------------------------
// Bar and beat grid over a waveform

void App::drawGrid(int t, int wx, int wy, uint32_t span, int w, int h) {
    const uint32_t bar = engine_.barFrames();
    const uint32_t beats = engine_.beatsPerBar() ? engine_.beatsPerBar() : 4;
    if (!bar || !span) return;
    const uint32_t beat = bar / beats;
    const uint32_t off = engine_.view(t).barOffset.load() % bar;   // first bar line in the buffer
    const float k = float(w) / float(span);
    const int wy1 = wy + h;
    // One LINES batch per colour: 2 display-list words per line.
    auto lines = [&](uint32_t from, uint32_t step, uint32_t cap, bool skipBars) {
        d_->lineWidth(16);
        d_->begin(Disp::kLines);
        words_ += 2;
        uint32_t count = 0;
        for (uint32_t i = from; i < span && count < cap; i += step) {
            if (i == 0 || (skipBars && i % bar == off)) continue;
            const int x = wx + int(float(i) * k);
            d_->vertex2f(x * 16, wy * 16);
            d_->vertex2f(x * 16, wy1 * 16);
            words_ += 2;
            ++count;
        }
        d_->end();
        ++words_;
    };
    if (beat && float(beat) * k >= 6.0f) {   // beats only when they are at least 6 px apart
        color(0x1C232D);
        lines(off % beat, beat, 96, true);
    }
    if (float(bar) * k >= 4.0f) {
        color(0x46505E);
        lines(off, bar, 64, false);
    }
    // Bar numbers: 1 at the take's start, then one per bar line.
    if (float(bar) * k >= 22.0f) {
        char buf[8];
        color(0x6B7685);
        int n = 1;
        if (off) text(wx + 3, wy + 1, kFontSmall, 0, "1"), ++n;
        for (uint32_t i = off; i < span && n <= 64; i += bar, ++n) {
            std::snprintf(buf, sizeof buf, "%d", n);
            text(wx + int(float(i) * k) + 3, wy + 1, kFontSmall, 0, buf);
        }
    }
}

// ---------------------------------------------------------------------------
// Performance overlay: eight cells over the waveform, one per pad column 5..12.

void App::drawPerfCells(int t, int wx, int wy) {
    const TrackView& v = engine_.view(t);
    const bool content = v.len.load() > 0;
    const uint8_t on = v.perf.load();
    const uint32_t tc = kTrackColor[t];
    for (int e = 0; e < kPerfFxCount; ++e) {
        const int x0 = wx + e * kWaveW / kPerfFxCount + 2, x1 = wx + (e + 1) * kWaveW / kPerfFxCount - 2;
        const int y0 = wy + 3, y1 = wy + kWaveH - 3;
        const bool active = content && ((on >> e) & 1);
        if (active) {
            color(tc);
            alpha(170);
        } else {
            color(0x000000);
            alpha(content ? 120 : 60);
        }
        fillRect(x0, y0, x1, y1, 6);
        alpha(255);
        // Name, and the effect's knob setting under it (shared by the tracks).
        char name[16], val[12];
        perfLabel(PerfFx(e), name, sizeof name);
        perfValue(PerfFx(e), val, sizeof val);
        const bool showVal = !(e == int(PerfFx::kRepeat8) || e == int(PerfFx::kRepeat16));   // already in the name
        color(active ? kBg : (content ? kInk : 0x3A4452));
        text((x0 + x1) / 2, (y0 + y1) / 2 - (showVal ? 8 : 0), kFontSmall, Disp::kOptCenter, name);
        if (showVal) {
            color(active ? kBg : kDim);
            text((x0 + x1) / 2, (y0 + y1) / 2 + 12, kFontSmall, Disp::kOptCenter, val);
        }
        if ((perfLatch_[t] >> e) & 1) {   // latched: a dot in the corner
            color(active ? kBg : tc);
            dot(x1 - 7, y0 + 7, 3);
        }
    }
}

// ---------------------------------------------------------------------------
// Track options page

bool App::optParam(int col, int row, OptParam& p, int& track) const {
    track = col < kTracks ? col : 0;
    static const OptParam kTrackRows[kOptRows] = {OptParam::kStop,  OptParam::kLength, OptParam::kStart,
                                                  OptParam::kInput, OptParam::kAfter,  OptParam::kStretch};
    static const OptParam kTempoRows[kOptRows] = {OptParam::kTempo, OptParam::kBeats, OptParam::kMetronome,
                                                  OptParam::kClick, OptParam::kThreshold, OptParam::kStopFade};
    if (row < 0 || row >= kOptRows) return false;
    if (col < kTracks) {
        p = kTrackRows[row];
        return true;
    }
    if (col == 4) {
        p = kTempoRows[row];
        return true;
    }
    return false;
}

void App::optKey(uint8_t id) {
    OptParam p;
    int t;
    if (id == b::F8) {
        goHome();
        return;
    }
    if (b::isFKey(id)) {
        if (id - b::F1 >= kOptCols) return;
        optCol_ = id - b::F1;
    } else if (id == b::Left && optCol_ > 0) --optCol_;
    else if (id == b::Right && optCol_ < kOptCols - 1) ++optCol_;
    else if (id == b::Up && optRow_ > 0) --optRow_;
    else if (id == b::Down && optRow_ < kOptRows - 1 && optParam(optCol_, optRow_ + 1, p, t)) ++optRow_;
    while (optRow_ > 0 && !optParam(optCol_, optRow_, p, t)) --optRow_;   // the tempo column is shorter
    if (optCol_ < kTracks) selected_ = optCol_;
}

void App::optEncoder(int delta) {
    OptParam p;
    int t;
    if (!optParam(optCol_, optRow_, p, t)) return;
    int v = engine_.opt(p, t), lo = 0, hi = 1, step = 1;
    switch (p) {
        case OptParam::kStop: hi = 2; break;
        case OptParam::kLength: hi = kMaxLenBeats; break;
        case OptParam::kStart: hi = 2; break;
        case OptParam::kInput: hi = 3; break;
        case OptParam::kAfter: hi = 2; break;
        case OptParam::kStretch: hi = 1; break;
        case OptParam::kStopFade: hi = 3; break;
        case OptParam::kMonitorFx: hi = 3; break;   // on the Input page, not here
        case OptParam::kTempo: tempoRange(lo, hi); break;
        case OptParam::kBeats: lo = 1; hi = 12; break;
        case OptParam::kMetronome: hi = 1; break;
        case OptParam::kClick: hi = 100; step = shift_ ? 1 : 5; break;
        case OptParam::kThreshold: hi = 100; step = shift_ ? 1 : 2; break;   // 0.6 / 1.2 dB steps
    }
    if (p == OptParam::kLength && !shift_) {
        // Whole bars; Shift moves single beats.
        const int bar = engine_.beatsPerBar() ? engine_.beatsPerBar() : 4;
        for (int i = 0; i < (delta < 0 ? -delta : delta); ++i)
            v = delta > 0 ? (v / bar + 1) * bar : ((v + bar - 1) / bar - 1) * bar;
    } else {
        v += delta * step;
    }
    v = v < lo ? lo : (v > hi ? hi : v);
    engine_.post(Engine::kOpt, uint8_t(t), int16_t((int(p) << 8) | v));
}

void App::drawOptions() {
    for (int c = 0; c < kOptCols; ++c) drawOptColumn(c);
    // Over F6..F8: what the selected option does.
    static const char* const kHelp[12][4] = {
        {"Stop", "Free: the take ends when you", "press. On bar / on beat: it", "records on to the next line."},
        {"Length", "Recording stops by itself.", "Encoder: whole bars,", "Shift + encoder: beats."},
        {"Start", "Now, or a bar of click first", "(count-in), or when the input", "goes over the threshold."},
        {"Input", "Mono takes half the memory,", "so twice the time. Change it", "while the track is empty."},
        {"After rec", "What the track does when", "the take ends: play, overdub", "or stop."},
        {"Tempo", "Sets the bar until there is a", "loop; after that it follows", "the loop's bar."},
        {"Beats / bar", "The click, on-beat stops and", "the beat lines on the", "waveforms."},
        {"Metronome", "Click on every beat while the", "transport runs (Shift+Play).", "The count-in always clicks."},
        {"Click", "Click level.", "", ""},
        {"Threshold", "Input level that starts an", "\"on sound\" take. Watch the", "armed lane's meter."},
        {"Tempo", "Tape: a tempo change moves", "the pitch too. Stretch: the", "track keeps its pitch."},
        {"Stop fade", "Stop fades the track out over", "this long, then stops it. A", "second press stops at once."},
    };
    OptParam p;
    int t;
    if (!optParam(optCol_, optRow_, p, t)) return;
    const int h = int(p);
    const int x = 5 * kKeyW + 12;
    color(kInk);
    text(x, 60, kFontSmall, 0, kHelp[h][0]);
    color(kDim);
    for (int i = 1; i < 4; ++i) text(x, 60 + 22 * i, kFontSmall, 0, kHelp[h][i]);
}

void App::drawOptColumn(int c) {
    static const char* const kRowName[kOptRows] = {"Stop", "Length", "Start", "Input", "After rec", "Tempo"};
    static const char* const kTempoRow[kOptRows] = {"Tempo", "Beats/bar", "Metronome", "Click", "Threshold",
                                                    "Stop fade"};
    const int x0 = c * kKeyW + 4, x1 = (c + 1) * kKeyW - 4, cx = c * kKeyW + kKeyW / 2;
    const uint32_t sc = c < kTracks ? kTrackColor[c] : kAmber;
    const bool sel = optCol_ == c;
    const bool empty = c < kTracks && (TrackState(engine_.view(c).state.load()) == TrackState::kEmpty ||
                                       TrackState(engine_.view(c).state.load()) == TrackState::kClearing);
    const bool haveLoop = engine_.masterLen() != 0;
    char buf[20];

    panel(x0, 6, x1, 428, 8, sel ? kLaneSel : kLane);
    color(scale(sc, 0.85f));
    fillRect(x0 + 4, 10, x1 - 4, 36, 6);
    color(kBg);
    if (c < kTracks) std::snprintf(buf, sizeof buf, "Track %d", c + 1);
    else std::snprintf(buf, sizeof buf, "Tempo");
    text(cx, 23, kFontSmall, Disp::kOptCenter, buf);

    for (int row = 0; row < kOptRows; ++row) {
        OptParam p;
        int t;
        if (!optParam(c, row, p, t)) continue;
        const int by0 = 44 + row * 64, by1 = by0 + 58;
        if (sel && optRow_ == row) {
            color(sc);
            fillRect(x0 + 2, by0 - 2, x1 - 2, by1 + 2, 8);
        }
        well(x0 + 4, by0, x1 - 4, by1, 6, kCell);
        const uint8_t v = engine_.opt(p, t);
        bool locked = false;
        const char* note = nullptr;
        char noteBuf[20];
        switch (p) {
            case OptParam::kStop: {
                static const char* const n[3] = {"Free", "On bar", "On beat"};
                std::snprintf(buf, sizeof buf, "%s", n[v < 3 ? v : 0]);
                break;
            }
            case OptParam::kLength: {
                const int bar = engine_.beatsPerBar() ? engine_.beatsPerBar() : 4;
                if (!v) std::snprintf(buf, sizeof buf, "Off");
                else if (v % bar == 0) std::snprintf(buf, sizeof buf, "%d bar%s", v / bar, v / bar == 1 ? "" : "s");
                else std::snprintf(buf, sizeof buf, "%d beat%s", int(v), v == 1 ? "" : "s");
                break;
            }
            case OptParam::kStart: {
                static const char* const n[3] = {"Now", "Count-in", "On sound"};
                std::snprintf(buf, sizeof buf, "%s", n[v < 3 ? v : 0]);
                break;
            }
            case OptParam::kInput: {
                static const char* const n[4] = {"Stereo", "Mono L", "Mono R", "Mono sum"};
                std::snprintf(buf, sizeof buf, "%s", n[v < 4 ? v : 0]);
                locked = !empty;
                note = locked ? "clear first" : nullptr;
                break;
            }
            case OptParam::kAfter: {
                static const char* const n[3] = {"Play", "Overdub", "Stop"};
                std::snprintf(buf, sizeof buf, "%s", n[v < 3 ? v : 0]);
                break;
            }
            case OptParam::kStretch: std::snprintf(buf, sizeof buf, "%s", v ? "Stretch" : "Tape"); break;
            case OptParam::kStopFade: {
                static const char* const n[4] = {"Off", "1 beat", "1 bar", "2 bars"};
                std::snprintf(buf, sizeof buf, "%s", n[v & 3]);
                break;
            }
            case OptParam::kMonitorFx: std::snprintf(buf, sizeof buf, "%u", unsigned(v)); break;   // not a row here
            case OptParam::kTempo:
                std::snprintf(buf, sizeof buf, "%u bpm", unsigned(v));
                if (haveLoop && engine_.speed() != 1.0f) {   // tape speed: the loop's own tempo beside it
                    std::snprintf(noteBuf, sizeof noteBuf, "%d%% of %d", int(engine_.speed() * 100.0f + 0.5f),
                                  int(engine_.baseTempo() + 0.5f));
                    note = noteBuf;
                }
                break;
            case OptParam::kBeats: std::snprintf(buf, sizeof buf, "%u", unsigned(v)); break;
            case OptParam::kMetronome: std::snprintf(buf, sizeof buf, "%s", v ? "On" : "Off"); break;
            case OptParam::kClick: std::snprintf(buf, sizeof buf, "%u", unsigned(v)); break;
            case OptParam::kThreshold: std::snprintf(buf, sizeof buf, "%d dB", int(std::lround(thresholdDb(v)))); break;
        }
        color(kDim);
        text(cx, by0 + 12, kFontSmall, Disp::kOptCenter, c < kTracks ? kRowName[row] : kTempoRow[row]);
        color(locked ? kDim : kInk);
        text(cx, by0 + 31, kFontSmall, Disp::kOptCenter, buf);
        if (note) {
            color(0x4A5563);
            text(cx, by0 + 48, kFontSmall, Disp::kOptCenter, note);
        }
    }
}

// ---------------------------------------------------------------------------
// Config page

void App::markConfig() {
    cfgDirty_ = true;
    cfgDirtyMs_ = p_.clock->millis();
}

void App::applyConfig() {
    // Push the settings to the hardware and the engine (start-up, Config edits).
    d_->setBacklight(Disp::brightnessDuty(cfg_.brightness));
    if (p_.setHeadphones) p_.setHeadphones(cfg_.phones);
    if (p_.selectInput) p_.selectInput(cfg_.source, cfg_.gain);
    engine_.post(Engine::kMonitor, 0, cfg_.monitor);
}

// ---------------------------------------------------------------------------
// Input page: the input card and the metronome card

namespace {
constexpr int kInRows[2] = {6, 4};
const char* const kInName[2][6] = {{"Source", "Gain", "Channels", "Monitor", "Monitor FX", "Overdub feedback"},
                                   {"Metronome", "Click level", "Tempo", "Beats per bar", "", ""}};
const char* const kInHelp[2][6] = {
    {"Line in, or the mic input at low or high gain.", "Codec input gain. Watch the meter while you play.",
     "Stereo or mono takes for the selected track (set while the track is empty).",
     "Hear the input through the outputs.",
     "Hear the input through the effects too (full send). Takes are still recorded dry.",
     "How much of the loop stays under each overdub pass."},
    {"Click on every beat, accent on the bar line. Count-ins click either way.", "Click volume.",
     "Before the first loop: the bar length. After: tape speed, 50 to 200 %.", "Beats in a bar: the grid for stops, count-ins and the click.",
     "", ""}};
const char* const kMonFxName[4] = {"Off", "FX 1", "FX 2", "Both"};
const char* const kChName[4] = {"Stereo", "Mono L", "Mono R", "Mono sum"};
}  // namespace

void App::inputKey(uint8_t id) {
    auto flipMonitor = [&] {
        cfg_.monitor = engine_.monitoring() ? 0 : 1;
        applyConfig();
        markConfig();
    };
    switch (id) {
        case b::F1: inCol_ = 0; inRow_ = 0; inputEncoder(1); break;   // the quick keys step their setting
        case b::F2: inCol_ = 0; inRow_ = 3; flipMonitor(); break;
        case b::F3: inCol_ = 0; inRow_ = 2; inputEncoder(1); break;
        case b::F4: inCol_ = 1; inRow_ = 0; inputEncoder(engine_.opt(OptParam::kMetronome) ? -1 : 1); break;
        case b::F5: {
            inCol_ = 0;
            inRow_ = 5;
            int i = 0;
            while (i < int(sizeof kFeedbackSteps) && kFeedbackSteps[i] != engine_.feedback()) ++i;
            engine_.post(Engine::kFeedback, 0, kFeedbackSteps[(i + 1) % int(sizeof kFeedbackSteps)]);
            break;
        }
        case b::F6:
        case b::F8: goHome(); break;
        case b::Up:
            if (inRow_ > 0) --inRow_;
            break;
        case b::Down:
            if (inRow_ + 1 < kInRows[inCol_]) ++inRow_;
            break;
        case b::Left:
        case b::Right:
            inCol_ = id == b::Right ? 1 : 0;
            if (inRow_ >= kInRows[inCol_]) inRow_ = kInRows[inCol_] - 1;
            break;
        default: break;
    }
}

void App::inputEncoder(int delta) {
    auto opt = [&](OptParam p, int t, int v) { engine_.post(Engine::kOpt, uint8_t(t), int16_t((int(p) << 8) | (v & 0xFF))); };
    auto lim = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
    if (inCol_ == 0) {
        switch (inRow_) {
            case 0: {   // line, mic low, mic high
                static const uint8_t kSrc[3] = {0, 2, 3};
                int i = cfg_.source == 2 ? 1 : (cfg_.source == 3 ? 2 : 0);
                i = (i + (delta > 0 ? 1 : 2)) % 3;
                cfg_.source = kSrc[i];
                break;
            }
            case 1: cfg_.gain = uint8_t(lim(int(cfg_.gain) + delta * (shift_ ? 1 : 2), 0, 100)); break;
            case 2: {   // the engine keeps it only while the track is empty
                const int c = (int(engine_.opt(OptParam::kInput, selected_)) + (delta > 0 ? 1 : 3)) % 4;
                opt(OptParam::kInput, selected_, c);
                return;
            }
            case 3: cfg_.monitor = delta > 0 ? 1 : 0; break;
            case 4: opt(OptParam::kMonitorFx, 0, lim(int(engine_.opt(OptParam::kMonitorFx)) + (delta > 0 ? 1 : -1), 0, 3)); return;
            default:
                engine_.post(Engine::kFeedback, 0, int16_t(lim(int(engine_.feedback()) + delta * (shift_ ? 1 : 5), 0, 100)));
                return;
        }
        applyConfig();
        markConfig();
        return;
    }
    switch (inRow_) {
        case 0: opt(OptParam::kMetronome, 0, delta > 0 ? 1 : 0); break;
        case 1: opt(OptParam::kClick, 0, lim(int(engine_.opt(OptParam::kClick)) + delta * (shift_ ? 1 : 5), 0, 100)); break;
        case 2: {
            int lo, hi;
            tempoRange(lo, hi);
            opt(OptParam::kTempo, 0, lim(int(engine_.opt(OptParam::kTempo)) + delta, lo, hi));
            break;
        }
        default: opt(OptParam::kBeats, 0, lim(int(engine_.opt(OptParam::kBeats)) + delta, 1, 12)); break;
    }
}

void App::drawInput() {
    char buf[64];
    const bool empty = TrackState(engine_.view(selected_).state.load()) == TrackState::kEmpty;
    for (int c = 0; c < 2; ++c) {
        const int x0 = c ? 405 : 20, x1 = c ? 780 : 395;
        color(kAmber);
        text(x0, 22, kFontBig, Disp::kOptCenterY, c ? "METRONOME" : "INPUT");
        for (int r = 0; r < kInRows[c]; ++r) {
            const int y0 = 46 + r * 48, y1 = y0 + 40, cy = (y0 + y1) / 2;
            const bool sel = inCol_ == c && inRow_ == r;
            if (sel) {
                color(kAmber);
                fillRect(x0 - 2, y0 - 2, x1 + 2, y1 + 2, 9);
            }
            panel(x0, y0, x1, y1, 8, sel ? kLaneSel : kLane);
            const char* name = kInName[c][r];
            if (c == 0 && r == 2) {
                std::snprintf(buf, sizeof buf, "Track %d channels", selected_ + 1);
                name = buf;
            }
            color(sel ? kInk : kDim);
            text(x0 + 16, cy, kFontBody, Disp::kOptCenterY, name);
            char v[24];
            float frac = -1.0f, meter = -1.0f;
            bool dim = false;
            if (c == 0) {
                switch (r) {
                    case 0: std::snprintf(v, sizeof v, "%s", sourceName(cfg_.source)); break;
                    case 1:
                        std::snprintf(v, sizeof v, "%u", unsigned(cfg_.gain));
                        frac = cfg_.gain / 100.0f;
                        meter = inMeter_[0] > inMeter_[1] ? inMeter_[0] : inMeter_[1];
                        break;
                    case 2:
                        std::snprintf(v, sizeof v, "%s", kChName[engine_.opt(OptParam::kInput, selected_) & 3]);
                        dim = !empty;   // fixed once the track holds a loop
                        break;
                    case 3: std::snprintf(v, sizeof v, "%s", engine_.monitoring() ? "On" : "Off"); break;
                    case 4:
                        std::snprintf(v, sizeof v, "%s", kMonFxName[engine_.opt(OptParam::kMonitorFx) & 3]);
                        dim = !engine_.monitoring();   // only with monitoring on
                        break;
                    default:
                        std::snprintf(v, sizeof v, "%u %%", unsigned(engine_.feedback()));
                        frac = engine_.feedback() / 100.0f;
                        break;
                }
            } else {
                switch (r) {
                    case 0: std::snprintf(v, sizeof v, "%s", engine_.opt(OptParam::kMetronome) ? "On" : "Off"); break;
                    case 1: {
                        const uint8_t k = engine_.opt(OptParam::kClick);
                        std::snprintf(v, sizeof v, "%u", unsigned(k));
                        frac = k / 100.0f;
                        break;
                    }
                    case 2:
                        if (engine_.masterLen() && engine_.speed() != 1.0f)
                            std::snprintf(v, sizeof v, "%u bpm  %d%%", unsigned(engine_.opt(OptParam::kTempo)),
                                          int(engine_.speed() * 100.0f + 0.5f));
                        else std::snprintf(v, sizeof v, "%u bpm", unsigned(engine_.opt(OptParam::kTempo)));
                        break;
                    default: std::snprintf(v, sizeof v, "%u", unsigned(engine_.opt(OptParam::kBeats))); break;
                }
            }
            if (frac >= 0.0f) {   // a level bar under the value
                const int bx0 = x1 - 150, bx1 = x1 - 16;
                color(kFaint);
                fillRect(bx0, y1 - 9, bx1, y1 - 6, 2);
                color(kAmber);
                fillRect(bx0, y1 - 9, bx0 + 2 + int(float(bx1 - bx0 - 2) * frac), y1 - 6, 2);
                if (meter > 0.005f) {   // the live input level just above it
                    color(meter > 0.9f ? kRed : (meter > 0.6f ? kAmber : 0x3DDC97));
                    fillRect(bx0, y1 - 13, bx0 + int(float(bx1 - bx0) * (meter > 1.0f ? 1.0f : meter)), y1 - 11, 1);
                }
            }
            color(dim ? kDim : kInk);
            text(x1 - 16, frac >= 0.0f ? cy - 4 : cy, kFontBody, Disp::kOptRightX | Disp::kOptCenterY, v);
        }
    }
    // What the selected setting does.
    color(kDim);
    const char* help = kInHelp[inCol_][inRow_];
    if (inCol_ == 0 && inRow_ == 2 && !empty) help = "This track holds a loop: clear it to change its channels.";
    text(20, 340, kFontSmall, 0, help);
    color(0x4A5563);
    text(20, 384, kFontSmall, 0, "Up / Down: setting   Left / Right: card   Encoder: value (Shift: fine)");
}

namespace {
constexpr int kCfgRows = 6;
const char* const kCfgName[kCfgRows] = {"Display brightness", "Pad brightness", "Headphones",
                                        "Input",              "Input gain",     "Monitor input"};
}  // namespace

void App::configKey(uint8_t id) {
    if (fwPage_) {
        fwKey(id);
        return;
    }
    if (midiPage_) {
        midiKey(id);
        return;
    }
    if (id == b::F2) {   // the MIDI page
        midiPage_ = true;
        midiRow_ = 0;
        midi_.learn(-1);
    } else if (id == b::F1) {   // the firmware screen
        fwPage_ = true;
        fwMsg_ = nullptr;
        confirm_ = -1;
        fwScan();
    } else if (id == b::F8) goHome();
    else if (id == b::Up && cfgRow_ > 0) --cfgRow_;
    else if (id == b::Down && cfgRow_ < kCfgRows - 1) ++cfgRow_;
}

void App::configEncoder(int delta) {
    if (midiPage_) {   // the clock row: off / send / follow; the others: the row
        if (midiRow_ == 0) {
            int c = int(cfg_.midiClock) + (delta > 0 ? 1 : -1);
            cfg_.midiClock = uint8_t(c < 0 ? 0 : (c > 2 ? 2 : c));
            markConfig();
        } else {
            midiRow_ += delta > 0 ? 1 : -1;
            midiRow_ = midiRow_ < 1 ? 1 : (midiRow_ > kMidiActions ? kMidiActions : midiRow_);
        }
        return;
    }
    if (fwPage_) {   // the encoder picks an image
        fwSel_ += delta > 0 ? 1 : -1;
        fwSel_ = fwSel_ < 0 ? 0 : (fwSel_ >= fwCount_ ? (fwCount_ ? fwCount_ - 1 : 0) : fwSel_);
        confirm_ = -1;
        return;
    }
    auto step = [&](uint8_t& v, int s, int lo, int hi) {
        int x = int(v) + delta * s;
        v = uint8_t(x < lo ? lo : (x > hi ? hi : x));
    };
    switch (cfgRow_) {
        case 0: step(cfg_.brightness, 1, 0, 2); break;
        case 1: step(cfg_.pads, shift_ ? 1 : 10, 10, 100); break;
        case 2: step(cfg_.phones, shift_ ? 1 : 5, 0, 100); break;
        case 3: {   // line, mic low, mic high
            static const uint8_t kSrc[3] = {0, 2, 3};
            int i = cfg_.source == 2 ? 1 : (cfg_.source == 3 ? 2 : 0);
            i = (i + (delta > 0 ? 1 : 2)) % 3;
            cfg_.source = kSrc[i];
            break;
        }
        case 4: step(cfg_.gain, shift_ ? 1 : 2, 0, 100); break;
        case 5: cfg_.monitor = delta > 0 ? 1 : 0; break;
        default: return;
    }
    applyConfig();
    markConfig();
}

namespace {
// The names the update bootloader takes (the stock updater's filter, as in
// hal/fw_update.cpp): at least 15 characters, PolyendTrac*.ptf or mt*.hex.
bool isImageName(const char* n) {
    const size_t len = std::strlen(n);
    if (len < 15 || len >= 64) return false;   // App::FwImage::name
    auto endsWith = [&](const char* s) {
        const size_t m = std::strlen(s);
        if (len < m) return false;
        for (size_t i = 0; i < m; ++i) {
            char c = n[len - m + i];
            if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
            if (c != s[i]) return false;
        }
        return true;
    };
    return (std::strncmp(n, "PolyendTrac", 11) == 0 && endsWith(".ptf")) ||
           (std::strncmp(n, "mt", 2) == 0 && endsWith(".hex"));
}
}  // namespace

void App::fwScan() {
    fwCount_ = 0;
    if (!p_.storage || !files_.cardReady()) return;
    p_.storage->listDir(
        "/Firmware",
        [](const ot::IStorage::DirEntry& e, void* ctx) {
            App& a = *static_cast<App*>(ctx);
            if (e.isDir || !isImageName(e.name)) return true;
            if (a.fwCount_ >= kMaxImages) return false;
            FwImage& f = a.fw_[a.fwCount_++];
            std::snprintf(f.name, sizeof f.name, "%s", e.name);
            f.bytes = e.size;
            return true;
        },
        this);
    // By name (the card's order is the order the files were written).
    for (int i = 1; i < fwCount_; ++i)
        for (int j = i; j > 0 && std::strcmp(fw_[j - 1].name, fw_[j].name) > 0; --j) {
            const FwImage x = fw_[j];
            fw_[j] = fw_[j - 1];
            fw_[j - 1] = x;
        }
    // Start on the image this build came from, else the first.
    fwSel_ = 0;
    char mine[64];
    runningImage(mine, sizeof mine);
    for (int i = 0; i < fwCount_; ++i)
        if (std::strcmp(fw_[i].name, mine) == 0) fwSel_ = i;
}

void App::runningImage(char* out, size_t n) const {
    // "looper-0.1" -> PolyendTracLooperv01.ptf (the Makefile's image name).
    char digits[16];
    size_t d = 0;
    for (const char* c = p_.version; *c && d + 1 < sizeof digits; ++c)
        if (*c >= '0' && *c <= '9') digits[d++] = *c;
    digits[d] = '\0';
    std::snprintf(out, n, "PolyendTracLooperv%s.ptf", digits);
}

void App::fwKey(uint8_t id) {
    const uint32_t now = p_.clock->millis();
    switch (id) {
        case b::F1:   // flash: press twice
            if (!fwCount_ || files_.busy()) return;
            if (confirm_ == 0 && int32_t(confirmUntilMs_ - now) > 0) {
                confirm_ = -1;
                fwMsg_ = nullptr;
                if (!p_.firmwareUpdate || !p_.firmwareUpdate(fw_[fwSel_].name))
                    fwMsg_ = "The updater refused this image (nothing was changed)";
            } else {
                confirm_ = 0;
                confirmUntilMs_ = now + 2000;
            }
            return;
        case b::F2: fwScan(); fwMsg_ = nullptr; break;   // rescan the card
        case b::F7: fwPage_ = false; break;              // back to the settings
        case b::F8: goHome(); break;
        case b::Up:
            if (fwSel_ > 0) --fwSel_;
            break;
        case b::Down:
            if (fwSel_ + 1 < fwCount_) ++fwSel_;
            break;
        default: return;
    }
    confirm_ = -1;
}

void App::midiKey(uint8_t id) {
    switch (id) {
        case b::F1:   // learn: the next note / controller in does this row's action
            if (midiRow_ > 0) midi_.learn(midi_.learning() == midiRow_ - 1 ? -1 : midiRow_ - 1);
            break;
        case b::F2:
            if (midiRow_ > 0) {
                cfg_.midiMap[midiRow_ - 1] = MidiBinding{};
                markConfig();
            }
            break;
        case b::F7:
            midiPage_ = false;
            midi_.learn(-1);
            break;
        case b::F8: goHome(); break;
        case b::Up:
            if (midiRow_ > 0) --midiRow_;
            midi_.learn(-1);
            break;
        case b::Down:
            if (midiRow_ < kMidiActions) ++midiRow_;
            midi_.learn(-1);
            break;
        default: break;
    }
}

void App::midiAction(MidiAction a) {
    switch (a) {
        case MidiAction::kRec: record(selected_); break;
        case MidiAction::kPlayStop: engine_.post(Engine::kPlayStop, uint8_t(selected_)); break;
        case MidiAction::kUndo: undoDub(false); break;
        case MidiAction::kRedo: undoDub(true); break;
        case MidiAction::kReplace: record(selected_, true); break;
        case MidiAction::kNextTrack: selected_ = (selected_ + 1) % kTracks; break;
        case MidiAction::kPrevTrack: selected_ = (selected_ + kTracks - 1) % kTracks; break;
        case MidiAction::kPlayAll: engine_.post(Engine::kTransport); break;
        default: break;
    }
}

void App::drawMidi() {
    char buf[48];
    color(kAmber);
    text(20, 22, kFontBig, Disp::kOptCenterY, "MIDI");
    // Left: the clock and what is coming in.
    static const char* const kClock[3] = {"Off", "Send", "Follow"};
    const bool sel0 = midiRow_ == 0;
    if (sel0) {
        color(kAmber);
        fillRect(18, 44, 342, 92, 9);
    }
    panel(20, 46, 340, 90, 8, sel0 ? kLaneSel : kLane);
    color(sel0 ? kInk : kDim);
    text(36, 68, kFontBody, Disp::kOptCenterY, "Clock");
    color(kInk);
    text(324, 68, kFontBody, Disp::kOptRightX | Disp::kOptCenterY, kClock[cfg_.midiClock % 3]);
    color(kDim);
    const MidiClock mc = MidiClock(cfg_.midiClock);
    if (mc == MidiClock::kSend) text(36, 116, kFontSmall, 0, "24 a beat, Start / Stop with Play; DIN and USB");
    else if (mc == MidiClock::kFollow) {
        if (midi_.clockIn()) std::snprintf(buf, sizeof buf, "Following: %.1f bpm", double(midi_.followBpm()));
        else std::snprintf(buf, sizeof buf, "Waiting for a clock (DIN or USB)");
        text(36, 116, kFontSmall, 0, buf);
        text(36, 138, kFontSmall, 0, "Start / Stop run the loops, the tempo follows");
    } else {
        text(36, 116, kFontSmall, 0, "No clock in or out");
    }
    std::snprintf(buf, sizeof buf, "Last in: %s", midi_.lastMessage());
    text(36, 184, kFontSmall, 0, buf);
    color(0x4A5563);
    text(36, 300, kFontSmall, 0, "Controls: a note or a controller (any");
    text(36, 320, kFontSmall, 0, "channel; a footswitch's 127 presses).");
    text(36, 340, kFontSmall, 0, "F1 Learn, then press it. F2 clears.");
    // Right: the actions and what plays them.
    for (int a = 0; a < kMidiActions; ++a) {
        const int y0 = 46 + a * 46, y1 = y0 + 40, cy = (y0 + y1) / 2;
        const bool sel = midiRow_ == a + 1;
        const bool waiting = midi_.learning() == a;
        if (sel) {
            color(waiting ? kInk : kAmber);
            fillRect(358, y0 - 2, 782, y1 + 2, 9);
        }
        panel(360, y0, 780, y1, 8, sel ? kLaneSel : kLane);
        color(sel ? kInk : kDim);
        text(376, cy, kFontBody, Disp::kOptCenterY, kMidiActionName[a]);
        const MidiBinding& mb = cfg_.midiMap[a];
        if (waiting) std::snprintf(buf, sizeof buf, "press it...");
        else if (mb.type == 1) std::snprintf(buf, sizeof buf, "Note %u", unsigned(mb.num));
        else if (mb.type == 2) std::snprintf(buf, sizeof buf, "CC %u", unsigned(mb.num));
        else std::snprintf(buf, sizeof buf, "-");
        color(waiting ? kInk : (mb.type ? kInk : kDim));
        text(764, cy, kFontBody, Disp::kOptRightX | Disp::kOptCenterY, buf);
    }
}

void App::drawFirmware() {
    char buf[96];
    color(kAmber);
    text(20, 22, kFontBig, Disp::kOptCenterY, "FIRMWARE");
    char mine[64];
    runningImage(mine, sizeof mine);
    if (!files_.cardReady() || !fwCount_) {
        color(kDim);
        text(40, 120, kFontBody, 0, files_.cardReady() ? "No update images in /Firmware" : "No SD card");
        text(40, 150, kFontSmall, 0, "Copy a PolyendTrac*.ptf file into the card's Firmware folder, then F2 to look again.");
    }
    // Six rows, scrolled to keep the selection in view.
    constexpr int kRows = 6;
    const int first = fwSel_ < kRows ? 0 : fwSel_ - kRows + 1;
    for (int r = 0; r < kRows && first + r < fwCount_; ++r) {
        const int i = first + r;
        const int y0 = 46 + r * 46, y1 = y0 + 40;
        const bool sel = fwSel_ == i;
        if (sel) {
            color(kAmber);
            fillRect(18, y0 - 2, 782, y1 + 2, 9);
        }
        panel(20, y0, 780, y1, 8, sel ? kLaneSel : kLane);
        color(sel ? kInk : kDim);
        text(40, (y0 + y1) / 2, kFontBody, Disp::kOptCenterY, fw_[i].name);
        const bool running = std::strcmp(fw_[i].name, mine) == 0;
        const bool stock = std::strncmp(fw_[i].name, "PolyendTracker", 14) == 0;
        if (running || stock) {
            color(running ? kAmber : 0x3DDC97);
            text(560, (y0 + y1) / 2, kFontSmall, Disp::kOptRightX | Disp::kOptCenterY, running ? "this looper" : "stock");
        }
        color(sel ? kInk : kDim);
        std::snprintf(buf, sizeof buf, "%lu KB", static_cast<unsigned long>((fw_[i].bytes + 1023) / 1024));
        text(760, (y0 + y1) / 2, kFontBody, Disp::kOptRightX | Disp::kOptCenterY, buf);
    }
    if (fwCount_ > kRows) {
        color(kDim);
        std::snprintf(buf, sizeof buf, "%d of %d", fwSel_ + 1, fwCount_);
        text(760, 22, kFontSmall, Disp::kOptRightX | Disp::kOptCenterY, buf);
    }
    color(fwMsg_ ? kRed : kDim);
    text(40, 340, kFontSmall, 0,
         fwMsg_ ? fwMsg_ : "F1 twice flashes the selected image. The Tracker restarts into the updater.");
    color(kDim);
    text(40, 362, kFontSmall, 0, "Loops that are not saved are lost. To come back, flash the looper image again.");
    color(0x4A5563);
    text(760, 384, kFontSmall, Disp::kOptRightX, "Up / Down or encoder: image");
}

void App::drawConfig() {
    if (fwPage_) {
        drawFirmware();
        return;
    }
    if (midiPage_) {
        drawMidi();
        return;
    }
    char buf[96];
    color(kAmber);
    text(20, 22, kFontBig, Disp::kOptCenterY, "CONFIG");
    for (int r = 0; r < kCfgRows; ++r) {
        const int y0 = 46 + r * 46, y1 = y0 + 40;
        const bool sel = cfgRow_ == r;
        if (sel) {
            color(kAmber);
            fillRect(18, y0 - 2, 782, y1 + 2, 9);
        }
        panel(20, y0, 780, y1, 8, sel ? kLaneSel : kLane);
        color(sel ? kInk : kDim);
        text(40, (y0 + y1) / 2, kFontBody, Disp::kOptCenterY, kCfgName[r]);
        float frac = -1.0f;   // a bar for the levels
        bool off = false;
        switch (r) {
            case 0: {
                static const char* const n[3] = {"Low", "Medium", "High"};
                std::snprintf(buf, sizeof buf, "%s", n[cfg_.brightness % 3]);
                break;
            }
            case 1: std::snprintf(buf, sizeof buf, "%u %%", unsigned(cfg_.pads)); frac = cfg_.pads / 100.0f; break;
            case 2:
                std::snprintf(buf, sizeof buf, "%u", unsigned(cfg_.phones));
                frac = cfg_.phones / 100.0f;
                off = !p_.setHeadphones;   // the simulator has no headphone amp
                break;
            case 3: std::snprintf(buf, sizeof buf, "%s", sourceName(cfg_.source)); break;
            case 4: std::snprintf(buf, sizeof buf, "%u", unsigned(cfg_.gain)); frac = cfg_.gain / 100.0f; break;
            default: std::snprintf(buf, sizeof buf, "%s", cfg_.monitor ? "On" : "Off"); break;
        }
        if (frac >= 0.0f) {
            color(kFaint);
            fillRect(420, (y0 + y1) / 2 - 3, 640, (y0 + y1) / 2 + 3, 3);
            color(off ? kDim : kAmber);
            fillRect(420, (y0 + y1) / 2 - 3, 426 + int(214.0f * frac), (y0 + y1) / 2 + 3, 3);
        }
        color(off ? kDim : kInk);
        text(760, (y0 + y1) / 2, kFontBody, Disp::kOptRightX | Disp::kOptCenterY, buf);
    }
    // About
    const uint32_t cap = engine_.memory(0).capacity;
    const uint32_t st = cap / kSampleRate, mo = 2 * cap / kSampleRate;
    color(kDim);
    std::snprintf(buf, sizeof buf, "Tracker Looper %s", p_.version);
    text(40, 340, kFontSmall, 0, buf);
    std::snprintf(buf, sizeof buf, "Recording time per track: %lu s stereo, %lu s mono", static_cast<unsigned long>(st),
                  static_cast<unsigned long>(mo));
    text(40, 362, kFontSmall, 0, buf);
    text(40, 384, kFontSmall, 0, files_.cardReady() ? "SD card ready" : "No SD card: settings are not saved");
    color(0x4A5563);
    text(760, 362, kFontSmall, Disp::kOptRightX, "Up / Down: setting   Encoder: value");
}

// ---------------------------------------------------------------------------
// File page

void App::fileKey(uint8_t id) {
    const uint32_t now = p_.clock->millis();
    const int n = files_.projectCount();
    switch (id) {
        case b::F1: files_.save(false); break;
        case b::F2: files_.save(true); break;
        case b::F3:   // load: replaces the loops, so press twice
            if (n && !files_.busy()) {
                if (confirm_ == 2 && int32_t(confirmUntilMs_ - now) > 0) {
                    confirm_ = -1;
                    perf_ = false;
                    files_.load(files_.projectNumber(projSel_));
                } else {
                    confirm_ = 2;
                    confirmUntilMs_ = now + 2000;
                }
                return;
            }
            break;
        case b::F5: files_.exportTracks(uint8_t(1u << selected_)); break;
        case b::F6: files_.exportTracks(0x0F); break;
        case b::F8: goHome(); break;
        case b::Up:
            if (projSel_ > 0) --projSel_;
            break;
        case b::Down:
            if (projSel_ < n - 1) ++projSel_;
            break;
        default: break;
    }
    confirm_ = -1;
}

void App::drawFile() {
    char buf[64];
    color(kAmber);
    text(20, 22, kFontBig, Disp::kOptCenterY, "FILE");
    const bool card = files_.cardReady();
    color(card ? kDim : kRed);
    text(780, 22, kFontSmall, Disp::kOptRightX | Disp::kOptCenterY, card ? "SD card ready" : "No SD card");

    // Projects (left)
    panel(12, 42, 392, 428, 9, kLane);
    color(kDim);
    text(28, 60, kFontSmall, Disp::kOptCenterY, "PROJECTS");
    const int n = files_.projectCount();
    if (!n) {
        color(0x4A5563);
        text(202, 200, kFontSmall, Disp::kOptCenter, "None yet: F1 saves this one");
    }
    const int rows = 9;
    int first = projSel_ - rows / 2;
    if (first > n - rows) first = n - rows;
    if (first < 0) first = 0;
    for (int i = first; i < n && i < first + rows; ++i) {
        const int y0 = 76 + (i - first) * 38, y1 = y0 + 32;
        const bool sel = i == projSel_, cur = files_.projectNumber(i) == files_.current();
        well(24, y0, 380, y1, 6, sel ? scale(kAmber, 0.85f) : kCell);
        std::snprintf(buf, sizeof buf, "Project %03d", files_.projectNumber(i));
        color(sel ? kBg : kInk);
        text(40, (y0 + y1) / 2, kFontBody, Disp::kOptCenterY, buf);
        if (cur) {
            color(sel ? kBg : kAmber);
            text(364, (y0 + y1) / 2, kFontSmall, Disp::kOptRightX | Disp::kOptCenterY, "current");
        }
    }

    // Tracks and the job (right)
    panel(408, 42, 788, 428, 9, kLane);
    color(kDim);
    text(424, 60, kFontSmall, Disp::kOptCenterY, "TRACKS");
    for (int t = 0; t < kTracks; ++t) {
        const TrackView& v = engine_.view(t);
        const uint32_t len = v.len.load();
        const int y = 92 + t * 40;
        color(kTrackColor[t]);
        dot(436, y, 9);
        color(t == selected_ ? kInk : kDim);
        if (len) {
            char s[16];
            fmtSeconds(s, sizeof s, len);
            std::snprintf(buf, sizeof buf, "Track %d   %s   %s", t + 1, s, v.mono.load() ? "mono" : "stereo");
        } else {
            std::snprintf(buf, sizeof buf, "Track %d   empty", t + 1);
        }
        text(456, y, kFontSmall, Disp::kOptCenterY, buf);
    }
    const int sy = 270;
    if (files_.busy()) {
        color(kFaint);
        fillRect(424, sy + 30, 772, sy + 40, 5);
        color(kAmber);
        fillRect(424, sy + 30, 430 + int(342.0f * files_.progress()), sy + 40, 5);
    }
    color(files_.busy() ? kAmber : kInk);
    text(424, sy + 10, kFontSmall, Disp::kOptCenterY, files_.status()[0] ? files_.status() : "Ready");
    color(0x4A5563);
    text(424, 360, kFontSmall, 0, "Projects: Looper/Projects/NNN");
    text(424, 380, kFontSmall, 0, "Exports: Looper/Export/NNN/trackN.wav");
    if (confirm_ == 2 && int32_t(confirmUntilMs_ - p_.clock->millis()) > 0) {
        color(kAmber);
        text(400, 408, kFontBody, Disp::kOptCenter, "Press LOAD again: it replaces the loops you have");
    }
}

// ---------------------------------------------------------------------------
// Loop editor (S.Edit)

void App::editReset() {
    editA_ = 0;
    editB_ = engine_.view(editTrack_).len.load();
    editMarker_ = 0;
    confirm_ = -1;
}

uint32_t App::editUnit(bool fine) const {
    // The marker step: a bar, a beat, a sixteenth or 10 ms; Shift a quarter of it (free: 1 ms).
    const uint32_t bar = engine_.barFrames() ? engine_.barFrames() : 88200u;
    const uint32_t beat = bar / (engine_.beatsPerBar() ? engine_.beatsPerBar() : 4);
    uint32_t u = editSnap_ == 0 ? bar : editSnap_ == 1 ? beat : editSnap_ == 2 ? beat / 4 : kSampleRate / 100;
    if (fine) u = editSnap_ == 3 ? kSampleRate / 1000 : u / 4;
    return u ? u : 1;
}

void App::editKey(uint8_t id) {
    const int t = editTrack_;
    const uint32_t len = engine_.view(t).len.load();
    const uint32_t now = p_.clock->millis();
    // Cut and erase overwrite audio: the first press arms, the second (within 2 s) acts.
    auto confirmed = [&](int key) {
        if (confirm_ == key && int32_t(confirmUntilMs_ - now) > 0) {
            confirm_ = -1;
            return true;
        }
        confirm_ = key;
        confirmUntilMs_ = now + 2000;
        return false;
    };
    switch (id) {
        case b::F1: editMarker_ = 0; break;
        case b::F2: editMarker_ = 1; break;
        case b::F3: editSnap_ = uint8_t((editSnap_ + 1) % 4); break;
        case b::F4:   // trim: keep the selection
            if (len && editB_ > editA_ && (editA_ > 0 || editB_ < len))
                engine_.post(Engine::kEditTrim, uint8_t(t), 0, editA_, editB_);
            break;
        case b::F5:   // cut: remove the selection
            if (len && editB_ > editA_ && editB_ - editA_ < len && confirmed(4))
                engine_.post(Engine::kEditCut, uint8_t(t), 0, editA_, editB_);
            return;
        case b::F6:   // erase: silence the selection
            if (len && editB_ > editA_ && confirmed(5)) engine_.post(Engine::kEditErase, uint8_t(t), 0, editA_, editB_);
            return;
        case b::F7: engine_.post(Engine::kEditUndo, uint8_t(t)); break;
        case b::F8: goHome(); return;
        case b::Left: editMarker_ = 0; break;
        case b::Right: editMarker_ = 1; break;
        case b::Up:
        case b::Down:   // another track
            editTrack_ = (editTrack_ + (id == b::Up ? kTracks - 1 : 1)) % kTracks;
            selected_ = editTrack_;
            editValid_ = false;
            editReset();
            break;
        default: break;
    }
    confirm_ = -1;
}

void App::editEncoder(int delta) {
    // Move the selected marker by the snap unit, onto the snap grid.
    const uint32_t len = engine_.view(editTrack_).len.load();
    if (!len) return;
    const uint32_t u = editUnit(shift_);
    const uint32_t minSel = 64;
    uint32_t& m = editMarker_ ? editB_ : editA_;
    int64_t v = int64_t(m);
    if (delta > 0) v = (v / u + delta) * int64_t(u);
    else v = ((v + u - 1) / u + delta) * int64_t(u);
    if (editMarker_ == 0) {
        if (v > int64_t(editB_) - minSel) v = int64_t(editB_) - minSel;
        if (v < 0) v = 0;
    } else {
        if (v < int64_t(editA_) + minSel) v = int64_t(editA_) + minSel;
        if (v > int64_t(len)) v = len;
    }
    m = uint32_t(v);
    confirm_ = -1;
}

void App::fmtPos(char* buf, size_t n, uint32_t f) const {
    // bar.beat.sixteenth, counted from the loop start (1.1.1 = the start).
    const uint32_t bar = engine_.barFrames() ? engine_.barFrames() : 88200u;
    const uint32_t beats = engine_.beatsPerBar() ? engine_.beatsPerBar() : 4;
    const uint32_t beat = bar / beats, six = beat / 4 ? beat / 4 : 1;
    std::snprintf(buf, n, "%lu.%lu.%lu", static_cast<unsigned long>(f / bar + 1),
                  static_cast<unsigned long>((f % bar) / beat + 1), static_cast<unsigned long>((f % beat) / six + 1));
}

void App::drawEditPage(uint32_t now) {
    const int t = editTrack_;
    const TrackView& v = engine_.view(t);
    const uint32_t tc = kTrackColor[t];
    const uint32_t len = v.len.load();
    char buf[64], a[16], bpos[16];
    color(tc);
    std::snprintf(buf, sizeof buf, "EDIT  Track %d", t + 1);
    text(20, 22, kFontBig, Disp::kOptCenterY, buf);
    if (!len) {
        color(kDim);
        text(400, 200, kFontBig, Disp::kOptCenter, "This track has no loop yet");
        text(400, 236, kFontSmall, Disp::kOptCenter, "Up / Down: another track");
        return;
    }
    color(kDim);
    fmtSeconds(a, sizeof a, len);
    std::snprintf(buf, sizeof buf, "loop %s", a);
    text(780, 22, kFontSmall, Disp::kOptRightX | Disp::kOptCenterY, buf);
    if (v.busy.load() || engine_.editBusy()) {
        color(kAmber);
        text(560, 22, kFontSmall, Disp::kOptCenter, "working...");
    }

    // The waveform: the selection bright, the rest dim; grid, markers, playhead.
    const int wx = 20, wy = 48, wx1 = wx + kEditW, wy1 = wy + kEditH;
    well(wx - 4, wy - 4, wx1 + 4, wy1 + 4, 8, 0x050505);
    const int xa = wx + int(uint64_t(kEditW) * editA_ / len), xb = wx + int(uint64_t(kEditW) * editB_ / len);
    if (editValid_) {
        auto part = [&](int x0, int x1, float k) {
            if (x1 <= x0) return;
            scissor(x0, wy, x1 - x0, kEditH);
            color(scale(tc, k));
            d_->drawL8(int16_t(wx), int16_t(wy), kEditRamG, kEditW, kEditH, 1);
            words_ += 13;
        };
        part(wx, xa, 0.3f);
        part(xa, xb, 1.0f);
        part(xb, wx1, 0.3f);
        scissorOff();
    }
    drawGrid(t, wx, wy, len, kEditW, kEditH);
    color(tc);
    alpha(40);
    fillRect(xa, wy, xb, wy1, 0);
    alpha(255);
    for (int mk = 0; mk < 2; ++mk) {
        const int x = mk ? xb : xa;
        const bool sel = editMarker_ == mk;
        color(sel ? kInk : scale(kInk, 0.5f));
        vline(x, wy - 4, wy1 + 4, sel ? 40 : 24);
        fillRect(x - 7, wy1 + 4, x + 7, wy1 + 14, 3);   // the handle
    }
    const uint32_t pos = v.pos.load();
    color(kAmber);
    vline(wx + int(uint64_t(kEditW) * (pos % len) / len), wy, wy1, 16);

    // Selection readout
    fmtPos(a, sizeof a, editA_);
    fmtPos(bpos, sizeof bpos, editB_);
    char dur[16];
    fmtSeconds(dur, sizeof dur, editB_ - editA_);
    color(editMarker_ == 0 ? kInk : kDim);
    text(wx, wy1 + 34, kFontBody, 0, a);
    color(editMarker_ == 1 ? kInk : kDim);
    text(wx1, wy1 + 34, kFontBody, Disp::kOptRightX, bpos);
    std::snprintf(buf, sizeof buf, "selection %s", dur);
    color(tc);
    text(400, wy1 + 44, kFontBody, Disp::kOptCenter, buf);
    color(0x4A5563);
    text(400, 300, kFontSmall, Disp::kOptCenter, "Encoder moves the marker on the snap grid (Shift: finer).");
    text(400, 322, kFontSmall, Disp::kOptCenter, "TRIM keeps the selection (UNDO brings the rest back).");
    text(400, 344, kFontSmall, Disp::kOptCenter, "CUT removes it, ERASE silences it: press twice.");
    if (confirm_ >= 0 && int32_t(confirmUntilMs_ - now) > 0) {
        color(kAmber);
        text(400, 390, kFontBody, Disp::kOptCenter,
             confirm_ == 4 ? "Press CUT again to remove the selection" : "Press ERASE again to silence it");
    }
}

// ---------------------------------------------------------------------------
// Knobs

void App::arc(int cx, int cy, float r, float a0, float a1, int segs) {
    // A LINE_STRIP along the circle; angles in radians, y pointing down, so a
    // growing angle turns clockwise on the screen.
    d_->begin(Disp::kLineStrip);
    for (int i = 0; i <= segs; ++i) {
        const float a = a0 + (a1 - a0) * float(i) / float(segs);
        d_->vertex2f(int32_t((float(cx) + r * std::cos(a)) * 16.0f), int32_t((float(cy) + r * std::sin(a)) * 16.0f));
    }
    d_->end();
    words_ += uint32_t(segs) + 3;
}

void App::drawKnob(int cx, int cy, int r, float frac, float shown, uint32_t col, bool sel, float glow,
                   const char* name, const char* value) {
    // 270-degree knob from lower left to lower right. `shown` is the eased
    // value (the arc glides to `frac`); `glow` 0..1 lights a halo (the effect's
    // output level).
    (void)frac;
    constexpr float kA0 = 2.35619449f, kSpan = 4.71238898f;   // 135 deg, 270 deg
    if (shown < 0.0f) shown = 0.0f;
    if (shown > 1.0f) shown = 1.0f;
    if (glow > 0.02f) {
        color(col);
        alpha(uint8_t(glow > 1.0f ? 70 : 70.0f * glow));
        dot(cx, cy, r + 12);
        alpha(255);
    }
    if (sel) {
        color(col);
        alpha(140);
        d_->lineWidth(16);
        arc(cx, cy, float(r) + 9.0f, kA0, kA0 + kSpan, 24);
        alpha(255);
    }
    d_->lineWidth(5 * 16);
    color(0x262E3A);
    arc(cx, cy, float(r), kA0, kA0 + kSpan, 24);
    if (shown > 0.005f) {
        color(col);
        arc(cx, cy, float(r), kA0, kA0 + kSpan * shown, 2 + int(22.0f * shown));
    }
    words_ += 1;
    // Body and pointer
    color(0x1A212C);
    dot(cx, cy, r - 9);
    const float a = kA0 + kSpan * shown;
    color(kInk);
    d_->lineWidth(3 * 16);
    d_->begin(Disp::kLines);
    d_->vertex2f(int32_t((float(cx) + float(r - 24) * std::cos(a)) * 16.0f),
                 int32_t((float(cy) + float(r - 24) * std::sin(a)) * 16.0f));
    d_->vertex2f(int32_t((float(cx) + float(r - 11) * std::cos(a)) * 16.0f),
                 int32_t((float(cy) + float(r - 11) * std::sin(a)) * 16.0f));
    d_->end();
    words_ += 5;
    if (name) {
        color(sel ? col : kDim);
        text(cx, cy - r - 20, kFontSmall, Disp::kOptCenter, name);
    }
    if (value) {
        color(kInk);
        text(cx, cy + r + 8, kFontSmall, Disp::kOptCenter, value);
    }
}

bool App::grainPanel() const {
    if (!f5Held_ || engine_.mode(selected_) != PlayMode::kGranular) return false;
    return f5Used_ || p_.clock->millis() - f5DownMs_ >= 250;   // a tap does not flash it
}

void App::grainText(int knob, char* buf, size_t n) const {
    const uint8_t v = engine_.grainParam(selected_, knob);
    switch (knob) {
        case 0: std::snprintf(buf, n, "%u%%", unsigned(v)); break;
        case 1: std::snprintf(buf, n, "%d ms", int(std::lround(20.0f * std::pow(25.0f, float(v) / 100.0f)))); break;
        case 2: std::snprintf(buf, n, "%.1fx", double(1.0f + 7.0f * float(v) * float(v) / 10000.0f)); break;
        default: std::snprintf(buf, n, "%+d st", int(v) - kGrainPitchZero); break;
    }
}

void App::drawGrainPanel() {
    // F5 held on a Granular track: its four grain knobs on a plate over the lanes.
    const uint32_t tc = kTrackColor[selected_];
    color(0x000000);
    alpha(110);
    fillRect(132, 84, 668, 376, 22);
    alpha(255);
    panel(140, 90, 660, 360, 16, 0x2A2A2A);
    char buf[32];
    std::snprintf(buf, sizeof buf, "Track %d granular", selected_ + 1);
    color(tc);
    text(400, 114, kFontBig, Disp::kOptCenter, buf);
    for (int i = 0; i < kGrainKnobs; ++i) {
        const GrainKnob& gk = kGrainKnobInfo[i];
        const float fr = float(engine_.grainParam(selected_, i)) / float(gk.max);
        grainShown_[i] += (fr - grainShown_[i]) * 0.35f;
        grainText(i, buf, sizeof buf);
        drawKnob(205 + i * 130, 236, 44, fr, grainShown_[i], tc, grainSel_ == i, 0.0f, gk.name, buf);
    }
    color(kDim);
    text(400, 336, kFontSmall, Disp::kOptCenter, "F1-F4: knob   Encoder: value   Release F5: done");
}

void App::drawPopupKnob(const char* title, const char* value, float frac, float& shown, uint32_t col, float meter) {
    // A big knob over the middle of the screen while its control is held.
    shown += (frac - shown) * 0.35f;
    color(0x000000);
    alpha(110);
    fillRect(262, 84, 544, 376, 22);   // a wide soft shadow under the plate
    alpha(255);
    panel(270, 90, 530, 360, 16, 0x2A2A2A);
    color(col);
    text(400, 112, kFontBig, Disp::kOptCenter, title);
    drawKnob(400, 226, 72, frac, shown, col, false, meter, nullptr, nullptr);
    color(kInk);
    text(400, 330, kFontBig, Disp::kOptCenter, value);
}

// ---------------------------------------------------------------------------
// Performance knobs

void App::perfLabel(PerfFx fx, char* buf, size_t n) const {
    if (fx == PerfFx::kRepeat8 || fx == PerfFx::kRepeat16) {
        const uint8_t v = engine_.perfParam(fx);
        std::snprintf(buf, n, "Rpt %s", kRepeatNames[v < 6 ? v : 0]);
    } else {
        std::snprintf(buf, n, "%s", kPerfFxName[int(fx)]);
    }
}

void App::perfValue(PerfFx fx, char* buf, size_t n) const {
    const PerfKnob& kn = kPerfKnobs[int(fx)];
    const uint8_t v = engine_.perfParam(fx);
    if (kn.list) {
        std::snprintf(buf, n, "%s", kn.list[v <= kn.max ? v : 0]);
        return;
    }
    if (fx == PerfFx::kLowPass || fx == PerfFx::kHighPass) {
        const float hz = fx == PerfFx::kLowPass ? lowPassHz(v) : highPassHz(v);
        if (hz >= 1000.0f) std::snprintf(buf, n, "%d.%d kHz", int(hz / 1000.0f), int(hz / 100.0f) % 10);
        else std::snprintf(buf, n, "%d Hz", int(hz));
        return;
    }
    std::snprintf(buf, n, "%u", unsigned(v));
}

void App::drawPerfKnob() {
    const PerfFx fx = PerfFx(heldFx_);
    const PerfKnob& kn = kPerfKnobs[heldFx_];
    char title[24], val[16];
    perfLabel(fx, title, sizeof title);
    perfValue(fx, val, sizeof val);
    const float fr = kn.max ? float(engine_.perfParam(fx)) / float(kn.max) : 0.0f;
    drawPopupKnob(title, val, fr, perfShown_, kTrackColor[selected_], trackMeter_[selected_]);
    color(kDim);
    text(400, 140, kFontSmall, Disp::kOptCenter, kn.name);
}

// ---------------------------------------------------------------------------
// FX page

void App::fxKey(uint8_t id) {
    const int slot = fxPage_;
    const FxTypeInfo& ti = fxTypeInfo(engine_.fxType(slot));
    if (fxBar_) {   // the key under a type picks it; any other key closes the bar
        if (b::isFKey(id) && id - b::F1 < kFxTypeCount) {
            engine_.post(Engine::kFxType, uint8_t(slot), int16_t(id - b::F1));
            fxSel_ = 0;
        }
        fxBar_ = false;
        return;
    }
    auto exists = [&](int k) { return k == kFxKnobs || (k >= 0 && k < kFxKnobs && ti.knob[k].name); };
    switch (id) {
        case b::F1: fxBar_ = true; break;
        case b::F2: case b::F3: case b::F4: case b::F5: {   // Shift: the second bank
            const int kn = (shift_ ? kFxBank : 0) + id - b::F2;
            if (exists(kn)) {
                fxSel_ = kn;
                fxBank_ = kn / kFxBank;
            }
            break;
        }
        case b::F6: fxSel_ = kFxKnobs; break;
        case b::F7: fxPage_ = 1 - fxPage_; break;
        case b::F8: goHome(); break;
        case b::Left:
            for (int k = fxSel_ - 1; k >= 0; --k)
                if (exists(k)) {
                    fxSel_ = k;
                    break;
                }
            break;
        case b::Right:
            for (int k = fxSel_ + 1; k <= kFxKnobs; ++k)
                if (exists(k)) {
                    fxSel_ = k;
                    break;
                }
            break;
        default: break;
    }
    if (fxSel_ < kFxKnobs) fxBank_ = fxSel_ / kFxBank;
}

int App::fxBankOf(int slot) const {
    // Shift held shows the second bank (Shift + F2..F5 pick from it);
    // otherwise the selected knob's bank.
    const FxTypeInfo& ti = fxTypeInfo(engine_.fxType(slot));
    if (!ti.knob[kFxBank].name) return 0;   // a type without a second bank
    if (shift_) return 1;
    return fxSel_ < kFxKnobs ? fxSel_ / kFxBank : fxBank_;
}

void App::fxEncoder(int delta) {
    const int slot = fxPage_;
    if (fxSel_ >= kFxKnobs) {   // the return level
        const MixParam p = slot ? MixParam::kReturn2 : MixParam::kReturn1;
        int v = int(engine_.mix(p)) + delta * (shift_ ? 1 : 2);
        v = v < 0 ? 0 : (v > 100 ? 100 : v);
        engine_.post(Engine::kMix, 0, int16_t((int(p) << 8) | v));
        return;
    }
    stepFxKnob(slot, fxSel_, delta);
}

void App::drawFxPage(uint32_t now) {
    const int slot = fxPage_;
    const FxType type = engine_.fxType(slot);
    const FxTypeInfo& ti = fxTypeInfo(type);
    const uint32_t sc = stripColor(4 + slot);
    char buf[24];
    if (fxSel_ < kFxKnobs && !ti.knob[fxSel_].name) fxSel_ = kFxKnobs;   // the type has fewer knobs

    // Title and the effect's live picture
    std::snprintf(buf, sizeof buf, "FX %d", slot + 1);
    color(sc);
    text(20, 22, kFontBig, Disp::kOptCenterY, buf);
    color(kInk);
    text(84, 22, kFontBig, Disp::kOptCenterY, ti.name);
    if (engine_.fxBusy(slot)) {
        color(kDim);
        text(780, 22, kFontSmall, Disp::kOptRightX | Disp::kOptCenterY, "switching...");
    }
    color(0x05070A);
    fillRect(12, 42, 788, 214, 10);
    drawFxVisual(slot, now);

    // The knobs over F2..F5, the return over F6 (F1 opens the type bar).
    const int bank = fxBankOf(slot);
    for (int i = 0; i < kFxBank; ++i) {
        const int k = bank * kFxBank + i;
        if (!ti.knob[k].name) continue;
        const float fr = fxKnobFrac(slot, k);
        knobShown_[slot][k] += (fr - knobShown_[slot][k]) * 0.3f;   // the knobs glide
        fxKnobText(slot, k, buf, sizeof buf);
        drawKnob((i + 1) * kKeyW + kKeyW / 2, 326, 34, fr, knobShown_[slot][k], sc, fxSel_ == k, 0.0f,
                 ti.knob[k].name, buf);
    }
    const uint8_t ret = engine_.mix(slot ? MixParam::kReturn2 : MixParam::kReturn1);
    knobShown_[slot][kFxKnobs] += (float(ret) / 100.0f - knobShown_[slot][kFxKnobs]) * 0.3f;
    std::snprintf(buf, sizeof buf, "%u", unsigned(ret));
    drawKnob(5 * kKeyW + kKeyW / 2, 326, 34, float(ret) / 100.0f, knobShown_[slot][kFxKnobs], sc,
             fxSel_ == kFxKnobs, retMeter_[slot] * 1.5f, "Return", buf);
    color(0x4A5563);
    if (ti.knob[kFxBank].name) {   // two banks
        std::snprintf(buf, sizeof buf, "Knobs %d of 2", bank + 1);
        color(kDim);
        text(700, 280, kFontSmall, Disp::kOptCenter, buf);
        color(0x4A5563);
        text(700, 300, kFontSmall, Disp::kOptCenter, "Hold Shift:");
        text(700, 320, kFontSmall, Disp::kOptCenter, bank ? "these four" : "the other four");
        text(700, 340, kFontSmall, Disp::kOptCenter, "Sends: mixer");
    } else {
        text(700, 300, kFontSmall, Disp::kOptCenter, "Encoder: knob");
        text(700, 320, kFontSmall, Disp::kOptCenter, "Shift: fine");
        text(700, 340, kFontSmall, Disp::kOptCenter, "Sends: mixer");
    }
}

void App::drawFxVisual(int slot, uint32_t now) {
    // A picture of what the effect does, moving with the transport or its LFO.
    const FxType type = engine_.fxType(slot);
    const uint32_t sc = stripColor(4 + slot);
    const int x0 = 28, x1 = 772, y0 = 56, y1 = 200, mid = (y0 + y1) / 2, h = (y1 - y0) / 2 - 6;
    const float glow = retMeter_[slot];
    auto k = [&](int i) { return float(engine_.fxKnob(slot, i)); };
    char buf[32];
    switch (type) {
        case FxType::kDelay:
        case FxType::kPingPong: {
            // Echo taps over two bars: height = feedback^n, darker with less tone;
            // ping-pong alternates up (left) and down (right). A playhead runs on
            // the transport and lights the taps it passes.
            const uint32_t bar = engine_.barFrames() ? engine_.barFrames() : 88200u;
            const uint8_t div = engine_.fxKnob(slot, 0);
            const DelayDiv& dv = kDelayDivs[div < kDelayDivCount ? div : 0];
            const float win = 2.0f * float(bar);
            uint32_t dt = uint32_t(uint64_t(bar) * dv.num / dv.den);
            while (dt > Delay::kMaxFrames - 4) dt /= 2;
            const float fb = k(1) / 100.0f, tone = k(2) / 100.0f;
            const bool pp = type == FxType::kPingPong;
            const float head = float(engine_.transportFrame() % uint32_t(win)) / win;
            // grid: bar line in the middle, beats faint
            color(0x1C232D);
            const int beats = engine_.beatsPerBar() ? engine_.beatsPerBar() : 4;
            for (int i = 1; i < 2 * beats; ++i) vline(x0 + (x1 - x0) * i / (2 * beats), y0, y1, 16);
            color(0x3A4452);
            vline((x0 + x1) / 2, y0, y1, 16);
            hline(x0, x1, mid, 16);
            float amp = 1.0f;
            d_->lineWidth(5 * 16);
            for (int n = 0; n < 48 && amp > 0.02f; ++n) {
                const float tf = float(n) * float(dt) / win;
                if (tf >= 1.0f) break;
                const int x = x0 + int(float(x1 - x0) * tf);
                const float since = head - tf;   // just passed by the playhead: brighter
                const float lit = since >= 0.0f && since < 0.06f ? 1.0f - since / 0.06f : 0.0f;
                const float shade = n == 0 ? 1.0f : (0.35f + 0.65f * std::pow(tone, 0.5f * float(n)));
                color(n == 0 ? kInk : scale(sc, shade * (0.6f + 0.4f * lit)));
                const int hh = int(float(h) * amp);
                const bool down = pp && (n & 1);
                d_->begin(Disp::kLines);
                d_->vertex2f(x * 16, (down ? mid : mid - hh) * 16);
                d_->vertex2f(x * 16, (down ? mid + hh : (pp && n ? mid : mid + hh)) * 16);
                d_->end();
                words_ += 4;
                // Tap 0 is the dry hit; the first echo is full height, each later
                // one feedback times the one before.
                amp = n == 0 ? 0.9f : amp * fb;
            }
            color(kInk);
            alpha(120);
            vline(x0 + int(float(x1 - x0) * head), y0, y1, 24);
            alpha(255);
            std::snprintf(buf, sizeof buf, "%s of a bar, feedback %d%%", dv.name, int(k(1)));
            color(kDim);
            text(x1 - 4, y1 - 2, kFontSmall, Disp::kOptRightX, buf);
            break;
        }
        case FxType::kReverb: {
            // The decay tail: 64 bars falling to -60 dB at RT60, brighter with
            // more tone, breathing with the output level.
            const float rt = 0.3f * std::pow(30.0f, k(0) / 100.0f);
            const float win = rt * 1.25f;
            const float tone = k(1) / 100.0f, width = k(2) / 100.0f;
            const float breathe = 0.7f + 0.3f * (glow > 1.0f ? 1.0f : glow);
            d_->lineWidth(int((float(x1 - x0) / 64.0f - 3.0f) * 16.0f));
            for (int i = 0; i < 64; ++i) {
                const float tsec = win * float(i) / 64.0f;
                const float env = std::exp(-6.9f * tsec / rt);
                // fixed jitter, drifting slowly: the diffuse texture of a tail
                const uint32_t hsh = uint32_t(i) * 2654435761u + (now / 90) * 40503u;
                const float j = 0.75f + 0.25f * float((hsh >> 16) & 255) / 255.0f;
                const int hh = int(float(h) * env * j * breathe);
                const int hb = int(float(hh) * (0.55f + 0.45f * width));
                const int x = x0 + int(float(x1 - x0) * (float(i) + 0.5f) / 64.0f);
                color(scale(sc, 0.35f + 0.65f * tone * (1.0f - 0.5f * float(i) / 64.0f) + 0.15f));
                d_->begin(Disp::kLines);
                d_->vertex2f(x * 16, (mid - hh) * 16);
                d_->vertex2f(x * 16, (mid + hb) * 16);
                d_->end();
                words_ += 5;
            }
            std::snprintf(buf, sizeof buf, "decay %d.%d s", int(rt), int(rt * 10.0f) % 10);
            color(kDim);
            text(x1 - 4, y1 - 2, kFontSmall, Disp::kOptRightX, buf);
            break;
        }
        case FxType::kChorus:
        case FxType::kFlanger: {
            // The two LFOs (left in the slot colour, right white) moving at the
            // rate; depth is their height, width the gap between them.
            const bool fl = type == FxType::kFlanger;
            const float r0 = k(0) / 100.0f;
            const float rate = fl ? 0.05f * std::pow(40.0f, r0) : 0.1f * std::pow(50.0f, r0);
            const float depth = k(1) / 100.0f;
            const float width = fl ? 1.0f : k(2) / 100.0f;
            const float ph = 6.2831853f * rate * float(now) / 1000.0f;
            const float cycles = 2.0f;
            color(0x3A4452);
            hline(x0, x1, mid, 16);
            for (int side = 0; side < 2; ++side) {
                // Opaque: a translucent strip shows dots where its segments overlap.
                color(side ? scale(kInk, 0.55f) : sc);
                d_->lineWidth(3 * 16);
                d_->begin(Disp::kLineStrip);
                for (int i = 0; i <= 64; ++i) {
                    const float u = float(i) / 64.0f;
                    const float a = 6.2831853f * cycles * u - ph + (side ? 1.5707963f * width : 0.0f);
                    const int y = mid - int(float(h) * (0.15f + 0.85f * depth) * std::sin(a));
                    d_->vertex2f((x0 + int(float(x1 - x0) * u)) * 16, y * 16);
                }
                d_->end();
                words_ += 68;
            }
            std::snprintf(buf, sizeof buf, "rate %d.%02d Hz", int(rate), int(rate * 100.0f) % 100);
            color(kDim);
            text(x1 - 4, y1 - 2, kFontSmall, Disp::kOptRightX, buf);
            break;
        }
        case FxType::kClouds: {
            // The 4.1 s record buffer, newest on the right: each grain a bar
            // where it reads, as tall as its envelope now, as wide as it is.
            const CloudsFx& c = engine_.fxClouds(slot);
            clouds::GrainVisual gv[CloudsFx::kMaxGrains];
            const int ng = c.grains(gv, CloudsFx::kMaxGrains);
            const float head = float(c.writeHead()) / float(CloudsFx::kFrames);
            color(0x1C1C1C);
            hline(x0, x1, mid, 16);
            for (int i = 1; i < 4; ++i) vline(x0 + (x1 - x0) * i / 4, y0 + 8, y1 - 8, 16);   // one second apart
            for (int i = 0; i < ng; ++i) {
                float age = head - gv[i].position;
                if (age < 0.0f) age += 1.0f;
                const int gx = x1 - int(age * float(x1 - x0));
                const float env = 1.0f - std::fabs(2.0f * gv[i].phase - 1.0f);
                const int gw = 2 + int(gv[i].size * float(x1 - x0));
                const int gh = 4 + int(env * float(h));
                const int py = mid - int((gv[i].pan - 0.5f) * 40.0f);   // left grains higher
                color(sc);
                alpha(uint8_t(80 + 150 * env));
                fillRect(gx - gw / 2, py - gh / 2, gx + gw / 2 + 1, py + gh / 2, 2);
            }
            alpha(255);
            color(c.frozen() ? kInk : kDim);
            vline(x1, y0 + 4, y1 - 4, 24);   // the write head (still when frozen)
            std::snprintf(buf, sizeof buf, "%d grain%s", ng, ng == 1 ? "" : "s");
            color(kDim);
            text(x0 + 4, y0 + 2, kFontSmall, 0, buf);
            if (c.frozen()) {
                color(kInk);
                text(x1 - 8, y0 + 2, kFontSmall, Disp::kOptRightX, "FROZEN");
            }
            break;
        }
        default: break;
    }
}

void App::drawStats() {
    // Help toggles this: audio load, frame rate, display list size, SPI uploads.
    char a[48], bb[48];
    if (haveCpu_) std::snprintf(a, sizeof a, "CPU %lu%% (max %lu%%)  %lu fps",
                                static_cast<unsigned long>((cpuAvg_ + 5) / 10),
                                static_cast<unsigned long>((cpuMax_ + 5) / 10), static_cast<unsigned long>(fps_));
    else std::snprintf(a, sizeof a, "CPU n/a  %lu fps", static_cast<unsigned long>(fps_));
    const uint32_t dlb = p_.dlBytes ? p_.dlBytes() : 0;
    if (dlb) std::snprintf(bb, sizeof bb, "DL %lu/8192 B  up %lu KB/s", static_cast<unsigned long>(dlb),
                           static_cast<unsigned long>(lastUploadBytes_ / 1024));
    else std::snprintf(bb, sizeof bb, "DL ~%lu/2048 w  up %lu KB/s", static_cast<unsigned long>(lastWords_),
                       static_cast<unsigned long>(lastUploadBytes_ / 1024));
    color(0x000000);
    alpha(210);
    fillRect(560, 10, 788, 50, 6);
    alpha(255);
    color(kInk);
    text(780, 14, kFontSmall, Disp::kOptRightX, a);
    text(780, 32, kFontSmall, Disp::kOptRightX, bb);
}

}  // namespace looper
