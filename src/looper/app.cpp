#include "looper/app.h"

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

constexpr uint32_t kFrameMs = 16;           // ~60 fps cap
constexpr uint32_t kLedMs = 33;
constexpr uint32_t kRecWaveMs = 50;         // recording lanes redraw at 20 Hz (SPI budget)
constexpr uint32_t kPowerHoldMs = 1500;
constexpr uint32_t kHintMs = 1500;

// Geometry: four lanes, then the F-key label row along the bottom edge.
constexpr int kLaneX0 = 6, kLaneX1 = 794, kLaneY0 = 6, kLaneH = 100, kLaneGap = 6;
constexpr int kWaveX = 130, kWaveDy = 6;
constexpr int kKeyY0 = 436, kKeyY1 = 476, kKeyW = 100;   // F1..F8 cells, each over its key

// Palette (0xRRGGBB)
constexpr uint32_t kBg = 0x0B0E13, kLane = 0x131820, kLaneSel = 0x1A212C, kCell = 0x151B24;
constexpr uint32_t kInk = 0xE8EDF3, kDim = 0x6B7685, kFaint = 0x2A3340, kRed = 0xFF3B4E, kAmber = 0xFFB547;
constexpr uint32_t kTrackColor[kTracks] = {0xFF6B6B, 0xFFC857, 0x3DDC97, 0x4EA8FF};

constexpr uint8_t kFontSmall = 26, kFontBig = 28;
constexpr uint8_t kFeedbackSteps[] = {100, 90, 75, 50};

inline uint8_t r8(uint32_t c) { return uint8_t(c >> 16); }
inline uint8_t g8(uint32_t c) { return uint8_t(c >> 8); }
inline uint8_t b8(uint32_t c) { return uint8_t(c); }

uint32_t scale(uint32_t c, float k) {
    if (k < 0) k = 0;
    if (k > 1) k = 1;
    return (uint32_t(r8(c) * k) << 16) | (uint32_t(g8(c) * k) << 8) | uint32_t(b8(c) * k);
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

const char* stateName(TrackState s) {
    switch (s) {
        case TrackState::kClearing: return "CLEAR";
        case TrackState::kEmpty: return "EMPTY";
        case TrackState::kCountIn: return "COUNT";
        case TrackState::kRecording: return "REC";
        case TrackState::kPlaying: return "PLAY";
        case TrackState::kOverdubbing: return "DUB";
        case TrackState::kStopped: return "STOP";
    }
    return "";
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
    if (!p_.scratch || p_.scratchBytes < kWaveBytes) return false;

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
    if (p_.selectInput) p_.selectInput(source_, inputGain_);
    p_.audio->setCallback(&App::audioFill, this);
    return p_.audio->start();
}

void App::audioFill(int16_t* out, uint32_t frames, void* ctx) {
    App* a = static_cast<App*>(ctx);
    const int16_t* in = a->p_.lineIn ? a->p_.lineIn(a->p_.lineInCtx) : nullptr;
    a->engine_.render(out, in, frames);
}

void App::loop() {
    if (p_.service) p_.service();
    ot::InputEvent ev;
    while (p_.input->poll(ev)) onInput(ev);
    engine_.service();

    const uint32_t now = p_.clock->millis();
    if (powerHeld_ && now - powerDownMs_ >= kPowerHoldMs && p_.powerOff) {
        p_.leds->clear();
        p_.leds->show();
        p_.powerOff();
    }
    if (now - lastLedMs_ >= kLedMs) {
        lastLedMs_ = now;
        updateLeds(now);
    }
    if (p_.cpuLoad && now - lastCpuMs_ >= 1000) {
        lastCpuMs_ = now;
        haveCpu_ = p_.cpuLoad(cpuAvg_, cpuMax_);
    }
    if (now - lastFrameMs_ >= kFrameMs && d_->frameReady()) {
        lastFrameMs_ = now;
        updateWaves(now);
        render(now);
    }
}

// ---------------------------------------------------------------------------
// Input

void App::hint(int t, const char* msg, uint32_t now) {
    hintTrack_ = t;
    hintText_ = msg;
    hintUntilMs_ = now + kHintMs;
}

void App::record(int t) {
    const TrackState s = TrackState(engine_.view(t).state.load());
    const ModeInfo& mi = modeInfo(engine_.mode(t));
    if ((s == TrackState::kPlaying || s == TrackState::kStopped) && !mi.canDub) {
        hint(t, "Overdub needs Forward, Reverse or Ping-pong", p_.clock->millis());
        return;
    }
    engine_.post(Engine::kRecord, uint8_t(t));
}

void App::onFKey(int key) {
    const uint8_t t = uint8_t(selected_);
    if (modeBar_) {   // the key under a mode label picks it
        engine_.post(Engine::kMode, t, int16_t(key));
        modeBar_ = false;
        return;
    }
    switch (key) {
        case 0: record(selected_); break;
        case 1: engine_.post(Engine::kPlayStop, t); break;
        case 2: engine_.post(Engine::kMute, t); break;
        case 3: engine_.post(shift_ ? Engine::kClearAll : Engine::kClear, t); break;
        case 4: modeBar_ = true; break;
        case 5:
            source_ = source_ == 0 ? 2 : (source_ == 2 ? 3 : 0);
            if (p_.selectInput) p_.selectInput(source_, inputGain_);
            break;
        case 6: engine_.post(Engine::kMonitor, 0, engine_.monitoring() ? 0 : 1); break;
        case 7: {
            int i = 0;
            while (i < int(sizeof kFeedbackSteps) && kFeedbackSteps[i] != engine_.feedback()) ++i;
            engine_.post(Engine::kFeedback, 0, kFeedbackSteps[(i + 1) % int(sizeof kFeedbackSteps)]);
            break;
        }
        default: break;
    }
}

void App::onInput(const ot::InputEvent& ev) {
    using E = ot::InputEvent;
    if (ev.type == E::kPadDown || ev.type == E::kPadUp) {
        onPad(ev.id, ev.type == E::kPadDown);
        return;
    }
    if (ev.type == E::kEncoder) {
        if (mixer_) {
            mixerEncoder(ev.delta);
            return;
        }
        if (options_) {
            optEncoder(ev.delta);
            return;
        }
        if (shift_) {
            int g = int(inputGain_) + ev.delta * 2;
            inputGain_ = uint8_t(g < 0 ? 0 : (g > 100 ? 100 : g));
            if (p_.selectInput) p_.selectInput(source_, inputGain_);
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
    if (ev.id == b::Performance) {
        // Tap: latch on (a second tap turns it off). Hold: on until released.
        if (down) {
            perfToggleOff_ = perf_;
            perf_ = true;
            perfDownMs_ = ev.timeMs;
            mixer_ = options_ = modeBar_ = false;
        } else if (perfToggleOff_ || ev.timeMs - perfDownMs_ >= 400) {
            closePerf();
        }
        return;
    }
    if (!down) return;
    if (ev.id == b::Master || ev.id == b::InstrParams) {   // page keys toggle their page
        const bool mix = ev.id == b::Master;
        mixer_ = mix && !mixer_;
        options_ = !mix && !options_;
        modeBar_ = false;
        return;
    }
    const bool nav = b::isFKey(ev.id) || ev.id == b::Left || ev.id == b::Right || ev.id == b::Up || ev.id == b::Down;
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
        case b::Rec: record(selected_); break;
        case b::Play: engine_.post(Engine::kTransport); break;
        case b::Delete:
            engine_.post(shift_ ? Engine::kClearAll : Engine::kClear, uint8_t(selected_));
            break;
        case b::Up: selected_ = (selected_ + kTracks - 1) % kTracks; break;
        case b::Down: selected_ = (selected_ + 1) % kTracks; break;
        case b::Help: stats_ = !stats_; break;
        default: break;
    }
}

void App::closePerf() {
    perf_ = false;
    perfToggleOff_ = false;
    for (uint8_t& l : perfLatch_) l = 0;
    engine_.post(Engine::kPerfOff);
}

void App::onPad(int pad, bool down) {
    if (pad < 0 || pad >= ot::kPadCount) return;
    const int t = pad / ot::kPadCols, col = pad % ot::kPadCols;
    if (t >= kTracks) return;
    if (perf_ && col >= 4) {
        // Performance: held effects, or Shift + pad to latch / unlatch.
        const int fx = col - 4;
        const uint8_t bit = uint8_t(1u << fx);
        if (down) {
            selected_ = t;
            bool on = true;
            if (shift_) {
                perfLatch_[t] ^= bit;
                on = (perfLatch_[t] & bit) != 0;
            }
            engine_.post(Engine::kPerf, uint8_t(t), int16_t((fx << 8) | (on ? 1 : 0)));
        } else if (!(perfLatch_[t] & bit)) {
            engine_.post(Engine::kPerf, uint8_t(t), int16_t(fx << 8));
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
            const uint8_t b = uint8_t(lv * 255.0f);
            L.setPad(uint8_t(row + col), b, b, b);
        };

        // Col 1 REC: fast blink recording, slow blink overdub, a flash per beat
        // in the count-in, faint otherwise so it can be found.
        float rec = kFaint;
        if (s == TrackState::kRecording) rec = fast ? kFull : kDimL;
        else if (s == TrackState::kOverdubbing) rec = slow ? kFull : kMid;
        else if (s == TrackState::kCountIn) rec = beat && (v.startIn.load() % beat) > beat / 2 ? kFull : kFaint;
        set(0, rec);
        set(1, s == TrackState::kPlaying || s == TrackState::kOverdubbing ? kMid : (content ? kFaint : kOff));
        set(2, muted ? kFull : (content ? kFaint : kOff));
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
// Waveform bitmaps (RGB332 intensity, tinted by COLOR_RGB at draw time)

void App::renderWave(int t, uint32_t span) {
    const TrackMemory& m = engine_.memory(t);
    const uint32_t written = engine_.view(t).written.load();
    const bool mono = engine_.view(t).mono.load();
    uint8_t* px = p_.scratch;
    std::memset(px, 0, kWaveBytes);
    const int mid = kWaveH / 2;
    const uint32_t cells = (written + kPeakFrames - 1) / kPeakFrames;
    // Intensity per distance from the centre line: bright core, darker edges.
    static uint8_t shade[kWaveH / 2 + 1];
    static bool shadeInit = false;
    if (!shadeInit) {
        for (int d = 0; d <= mid; ++d) {
            const int v = 255 - d * 150 / mid;
            shade[d] = uint8_t(((v >> 5) << 5) | ((v >> 5) << 2) | (v >> 6));
        }
        shadeInit = true;
    }
    // Stereo: the left channel grows up from the centre line, the right one down.
    // Mono tracks keep one peak byte per cell and draw it both ways.
    const uint8_t baseline = uint8_t((1 << 5) | (1 << 2));
    for (int x = 0; x < kWaveW; ++x) {
        const uint32_t c0 = uint32_t(uint64_t(span) * x / kWaveW / kPeakFrames);
        uint32_t c1 = uint32_t(uint64_t(span) * (x + 1) / kWaveW / kPeakFrames);
        if (c1 <= c0) c1 = c0 + 1;
        uint8_t pl = 0, pr = 0;
        for (uint32_t c = c0; c < c1 && c < cells; ++c) {
            const uint8_t a = mono ? m.peaks[c] : m.peaks[2 * c], bb = mono ? a : m.peaks[2 * c + 1];
            if (a > pl) pl = a;
            if (bb > pr) pr = bb;
        }
        px[mid * kWaveW + x] = baseline;
        if (c0 >= cells) continue;
        const int hl = pl ? 1 + pl * (mid - 2) / 255 : 0;
        const int hr = pr ? 1 + pr * (mid - 2) / 255 : 0;
        for (int dy = 1; dy <= hl; ++dy) px[(mid - dy) * kWaveW + x] = shade[dy];
        for (int dy = 1; dy <= hr; ++dy) px[(mid + dy) * kWaveW + x] = shade[dy];
    }
    d_->writeRamG(kWaveRamG + uint32_t(t) * kWaveBytes, px, kWaveBytes);
    uploadBytes_ += kWaveBytes;
}

void App::updateWaves(uint32_t now) {
    const uint32_t bar = engine_.barFrames();
    for (int t = 0; t < kTracks; ++t) {
        const TrackView& v = engine_.view(t);
        const TrackState s = TrackState(v.state.load());
        if (s == TrackState::kEmpty || s == TrackState::kClearing || s == TrackState::kCountIn) {
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
    }
}

// ---------------------------------------------------------------------------
// Drawing helpers

void App::color(uint32_t rgb) {
    d_->colorRGB(r8(rgb), g8(rgb), b8(rgb));
    ++words_;
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
    words_ += 3;

    if (mixer_) drawMixer();
    else if (options_) drawOptions();
    else
        for (int t = 0; t < kTracks; ++t) drawLane(t, now);
    drawFKeys();
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
    color(sel ? kLaneSel : kLane);
    fillRect(kLaneX0, y0, kLaneX1, y1, 9);

    // Badge and state pill
    const int cx = 30, cy = y0 + 24;
    color(empty ? scale(tc, 0.35f) : tc);
    dot(cx, cy, 15);
    color(kBg);
    std::snprintf(buf, sizeof buf, "%d", t + 1);
    text(cx, cy, kFontBig, Disp::kOptCenter, buf);
    uint32_t pc = kFaint, ink = kDim;
    switch (s) {
        case TrackState::kRecording: pc = scale(kRed, 0.55f + 0.45f * pulse(now, 700)); ink = 0xFFFFFF; break;
        case TrackState::kOverdubbing: pc = scale(kAmber, 0.55f + 0.45f * pulse(now, 700)); ink = 0x1A1200; break;
        case TrackState::kPlaying: pc = scale(tc, 0.85f); ink = kBg; break;
        case TrackState::kStopped: pc = 0x3A4452; ink = kInk; break;
        default: break;
    }
    color(pc);
    fillRect(54, y0 + 12, 120, y0 + 36, 12);
    color(ink);
    text(87, y0 + 24, kFontSmall, Disp::kOptCenter, stateName(s));

    // Mode (in the track colour when it is not plain forward), length, volume
    const bool mono = empty ? engine_.opt(OptParam::kInput, t) != uint8_t(InputCh::kStereo) : v.mono.load();
    color(mode == PlayMode::kForward ? kDim : tc);
    text(16, y0 + 44, kFontSmall, 0, modeInfo(mode).name);
    if (muted) {
        color(0xC0A000);
        text(120, y0 + 44, kFontSmall, Disp::kOptRightX, "MUTE");
    } else if (mono) {
        color(kDim);
        text(120, y0 + 44, kFontSmall, Disp::kOptRightX, "MONO");
    }
    color(kDim);
    if (len) fmtSeconds(buf, sizeof buf, len);
    else if (s == TrackState::kRecording) fmtSeconds(buf, sizeof buf, written);
    else std::snprintf(buf, sizeof buf, "--");
    text(16, y0 + 62, kFontSmall, 0, buf);
    const uint8_t vol = engine_.volume(t);
    std::snprintf(buf, sizeof buf, "%u", unsigned(vol));
    text(120, y0 + 62, kFontSmall, Disp::kOptRightX, buf);
    const int vx0 = 16, vx1 = 120, vy = y0 + 88;
    color(kFaint);
    fillRect(vx0, vy - 2, vx1, vy + 2, 2);
    color(scale(tc, 0.45f));
    fillRect(vx0, vy - 2, vx0 + 4 + (vx1 - vx0 - 4) * vol / 100, vy + 2, 2);
    if (trackMeter_[t] > 0.01f) {
        color(tc);
        fillRect(vx0, vy - 2, vx0 + 4 + int((vx1 - vx0 - 4) * trackMeter_[t] * vol / 100), vy + 2, 2);
    }

    // Waveform inset
    const int wx = kWaveX, wy = y0 + kWaveDy, wx1 = wx + kWaveW, wy1 = wy + kWaveH;
    color(0x05070A);
    fillRect(wx - 2, wy - 2, wx1 + 2, wy1 + 2, 6);
    if (s == TrackState::kRecording || s == TrackState::kCountIn) {
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

    if (s == TrackState::kCountIn) {
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
        const float dim = muted || s == TrackState::kStopped ? 0.35f : 1.0f;
        const bool rev = mode == PlayMode::kReverse && s != TrackState::kRecording;
        const float leftK = rev ? 0.45f * dim : dim, rightK = rev ? dim : 0.45f * dim;
        if (px > wx) {
            scissor(wx, wy, px - wx, kWaveH);
            color(scale(tc, leftK));
            d_->drawRgb332(int16_t(wx), int16_t(wy), ramg, kWaveW, kWaveH, 1);
            words_ += 13;
        }
        if (px < wx1) {
            scissor(px, wy, wx1 - px, kWaveH);
            color(scale(tc, rightK));
            d_->drawRgb332(int16_t(wx), int16_t(wy), ramg, kWaveW, kWaveH, 1);
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

    if (perf_ && !empty && s != TrackState::kCountIn) drawPerfCells(t, wx, wy);   // effects need a loop

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
    for (int i = 0; i < 8; ++i) {
        const int x0 = i * kKeyW + 3, x1 = (i + 1) * kKeyW - 3, cx = i * kKeyW + kKeyW / 2;
        bool hot = false;
        if (modeBar_) {
            title[i] = "";
            std::snprintf(val[i], sizeof val[i], "%s", kModes[i].name);
            hot = int(mode) == i;
        } else if (mixer_) {   // F1..F7 pick a strip, F8 goes back
            static const char* const kTitles[8] = {"TRACK", "TRACK", "TRACK", "TRACK", "FX 1", "FX 2", "MASTER", "BACK"};
            static const char* const kNames[8] = {"Track 1", "Track 2", "Track 3", "Track 4",
                                                   "Delay",   "Reverb",  "Level",   "Loops"};
            title[i] = kTitles[i];
            std::snprintf(val[i], sizeof val[i], "%s", kNames[i]);
            hot = strip_ == i;
        } else if (options_) {   // F1..F5 pick a column, F8 goes back
            static const char* const kTitles[8] = {"TRACK", "TRACK", "TRACK", "TRACK", "GLOBAL", "", "", "BACK"};
            static const char* const kNames[8] = {"Track 1", "Track 2", "Track 3", "Track 4", "Tempo", "", "", "Loops"};
            title[i] = kTitles[i];
            std::snprintf(val[i], sizeof val[i], "%s", kNames[i]);
            hot = optCol_ == i;
        } else {
            switch (i) {
                case 0:
                    title[i] = "REC";
                    std::snprintf(val[i], sizeof val[i], "%s",
                                  s == TrackState::kEmpty ? "Record"
                                  : s == TrackState::kCountIn ? "Cancel"
                                  : s == TrackState::kRecording ? "Finish"
                                  : s == TrackState::kOverdubbing ? "Stop dub"
                                  : s == TrackState::kClearing ? "-"
                                  : modeInfo(mode).canDub ? "Overdub" : "-");
                    break;
                case 1:
                    title[i] = "PLAY";
                    std::snprintf(val[i], sizeof val[i], "%s",
                                  s == TrackState::kPlaying || s == TrackState::kOverdubbing ? "Stop"
                                  : s == TrackState::kStopped ? "Play" : "-");
                    break;
                case 2:
                    title[i] = "MUTE";
                    std::snprintf(val[i], sizeof val[i], "%s", v.muted.load() ? "On" : "Off");
                    break;
                case 3:
                    title[i] = "CLEAR";
                    if (shift_) std::snprintf(val[i], sizeof val[i], "All");
                    else std::snprintf(val[i], sizeof val[i], "Track %c", char('1' + selected_));
                    break;
                case 4:
                    title[i] = "MODE";
                    std::snprintf(val[i], sizeof val[i], "%s", modeInfo(mode).name);
                    break;
                case 5:
                    title[i] = "INPUT";
                    std::snprintf(val[i], sizeof val[i], "%s", sourceName(source_));
                    break;
                case 6:
                    title[i] = "MONITOR";
                    std::snprintf(val[i], sizeof val[i], "%s", engine_.monitoring() ? "On" : "Off");
                    break;
                default:
                    title[i] = "FEEDBACK";
                    std::snprintf(val[i], sizeof val[i], "%u%%", unsigned(engine_.feedback()));
                    break;
            }
        }
        uint32_t hc = tc;
        if (mixer_ && !modeBar_) hc = stripColor(i);
        else if (options_ && !modeBar_) hc = i < kTracks ? kTrackColor[i] : (i == 4 ? kAmber : kDim);
        color(hot ? hc : kCell);
        fillRect(x0, kKeyY0, x1, kKeyY1, 6);
        if (title[i][0]) {
            uint32_t tcol = kDim;
            if (hot) tcol = kBg;
            else if (mixer_) tcol = i < 7 ? scale(hc, 0.9f) : kDim;
            else if (options_) tcol = i < 5 ? scale(hc, 0.9f) : kDim;
            else if (!modeBar_ && i < 5) tcol = scale(tc, 0.9f);
            color(tcol);
            text(cx, kKeyY0 + 10, kFontSmall, Disp::kOptCenter, title[i]);
        }
        color(hot ? kBg : kInk);
        text(cx, modeBar_ ? (kKeyY0 + kKeyY1) / 2 + 4 : kKeyY0 + 28, kFontSmall,
             Disp::kOptCenter, val[i]);
    }
    // Input level inside the INPUT cell
    if (!modeBar_ && !mixer_ && !options_) {
        const int x0 = 5 * kKeyW + 10, x1 = 6 * kKeyW - 10;
        for (int c = 0; c < 2; ++c) {
            const float m = inMeter_[c];
            if (m < 0.005f) continue;
            color(m > 0.9f ? kRed : (m > 0.6f ? kAmber : 0x3DDC97));
            hline(x0, x0 + int((x1 - x0) * m), kKeyY1 - 5 + c * 2, 16);
        }
    }
}

// ---------------------------------------------------------------------------
// Mixer page

bool App::mixParam(int strip, int row, MixParam& p, int& track) const {
    track = strip < kTracks ? strip : 0;
    if (strip < kTracks) {
        p = row == 0 ? MixParam::kSend1 : (row == 1 ? MixParam::kSend2 : MixParam::kLevel);
        return true;
    }
    if (strip == 4) {
        p = row == 0 ? MixParam::kDelayTime : (row == 1 ? MixParam::kDelayFeedback : MixParam::kReturn1);
        return true;
    }
    if (strip == 5) {
        p = row == 0 ? MixParam::kReverbSize : (row == 1 ? MixParam::kReverbTone : MixParam::kReturn2);
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
    int t;
    if (id == b::F8) {
        mixer_ = false;
        return;
    }
    if (b::isFKey(id)) strip_ = id - b::F1;
    else if (id == b::Left && strip_ > 0) --strip_;
    else if (id == b::Right && strip_ < kStrips - 1) ++strip_;
    else if (id == b::Up && row_ > 0 && mixParam(strip_, row_ - 1, p, t)) --row_;
    else if (id == b::Down && row_ < kRows - 1) ++row_;
    if (!mixParam(strip_, row_, p, t)) row_ = kRows - 1;   // master has only the fader
    if (strip_ < kTracks) selected_ = strip_;
}

void App::mixerEncoder(int delta) {
    MixParam p;
    int t;
    if (!mixParam(strip_, row_, p, t)) return;
    const int step = p == MixParam::kDelayTime ? 1 : (shift_ ? 1 : 2);
    const int hi = p == MixParam::kDelayTime ? kDelayDivCount - 1 : (p == MixParam::kDelayFeedback ? 95 : 100);
    int v = int(engine_.mix(p, t)) + delta * step;
    v = v < 0 ? 0 : (v > hi ? hi : v);
    engine_.post(Engine::kMix, uint8_t(t), int16_t((int(p) << 8) | v));
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
    static const char* const kStripName[kStrips] = {"Track 1", "Track 2", "Track 3", "Track 4",
                                                     "Delay",   "Reverb",  "Master"};
    static const char* const kRowName[kStrips][2] = {
        {"To delay", "To reverb"}, {"To delay", "To reverb"}, {"To delay", "To reverb"},
        {"To delay", "To reverb"}, {"Time", "Feedback"},     {"Size", "Tone"},
        {nullptr, nullptr}};
    const int x0 = s * kKeyW + 4, x1 = (s + 1) * kKeyW - 4, cx = s * kKeyW + kKeyW / 2;
    const uint32_t sc = stripColor(s);
    const bool sel = strip_ == s;
    const bool emptyTrack = s < kTracks && TrackState(engine_.view(s).state.load()) == TrackState::kEmpty;
    char buf[16];

    color(sel ? kLaneSel : kLane);
    fillRect(x0, 6, x1, 428, 8);
    color(scale(sc, emptyTrack ? 0.35f : 0.85f));
    fillRect(x0 + 4, 10, x1 - 4, 36, 6);
    color(emptyTrack ? kDim : kBg);
    text(cx, 23, kFontSmall, Disp::kOptCenter, kStripName[s]);

    auto valueText = [&](MixParam p, uint8_t v) {
        if (p == MixParam::kDelayTime) std::snprintf(buf, sizeof buf, "%s", kDelayDivs[v < kDelayDivCount ? v : 0].name);
        else if (p == MixParam::kDelayFeedback) std::snprintf(buf, sizeof buf, "%u%%", unsigned(v));
        else std::snprintf(buf, sizeof buf, "%u", unsigned(v));
    };
    auto frac = [](MixParam p, uint8_t v) {
        if (p == MixParam::kDelayTime) return float(v) / float(kDelayDivCount - 1);
        return float(v) / (p == MixParam::kDelayFeedback ? 95.0f : 100.0f);
    };
    auto outline = [&](int bx0, int by0, int bx1, int by1) {
        color(sc);
        fillRect(bx0 - 2, by0 - 2, bx1 + 2, by1 + 2, 8);
    };

    // Two small parameters: label, value, bar. Track sends use their effect's colour.
    for (int row = 0; row < 2; ++row) {
        MixParam p;
        int t;
        if (!mixParam(s, row, p, t)) continue;
        const int by0 = 46 + row * 68, by1 = by0 + 60;
        if (sel && row_ == row) outline(x0 + 4, by0, x1 - 4, by1);
        color(kCell);
        fillRect(x0 + 4, by0, x1 - 4, by1, 6);
        const uint8_t v = engine_.mix(p, t);
        color(kDim);
        text(cx, by0 + 12, kFontSmall, Disp::kOptCenter, kRowName[s][row]);
        valueText(p, v);
        color(kInk);
        text(cx, by0 + 31, kFontSmall, Disp::kOptCenter, buf);
        const uint32_t bc = s < kTracks ? (row == 0 ? kDelayColor : kReverbColor) : sc;
        color(kFaint);
        fillRect(x0 + 12, by0 + 47, x1 - 12, by0 + 51, 2);
        color(scale(bc, emptyTrack ? 0.4f : 1.0f));
        fillRect(x0 + 12, by0 + 47, x0 + 16 + int((x1 - x0 - 28) * frac(p, v)), by0 + 51, 2);
    }

    // The fader: level, return or master, with its meter beside it.
    MixParam p;
    int t;
    if (!mixParam(s, 2, p, t)) return;
    const int fy0 = 184, fy1 = 424;
    if (sel && row_ == 2) outline(x0 + 4, fy0, x1 - 4, fy1);
    color(kCell);
    fillRect(x0 + 4, fy0, x1 - 4, fy1, 6);
    color(kDim);
    text(cx, fy0 + 12, kFontSmall, Disp::kOptCenter, s < kTracks || s == 6 ? "Level" : "Return");
    const uint8_t v = engine_.mix(p, t);
    const int sy0 = fy0 + 30, sy1 = fy1 - 34;           // slot
    const int ky = sy1 - int((sy1 - sy0) * frac(p, v));  // knob
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
    valueText(p, v);
    color(kInk);
    text(cx, fy1 - 16, kFontSmall, Disp::kOptCenter, buf);
}

// ---------------------------------------------------------------------------
// Bar and beat grid over a waveform

void App::drawGrid(int t, int wx, int wy, uint32_t span) {
    const uint32_t bar = engine_.barFrames();
    const uint32_t beats = engine_.beatsPerBar() ? engine_.beatsPerBar() : 4;
    if (!bar || !span) return;
    const uint32_t beat = bar / beats;
    const uint32_t off = engine_.view(t).barOffset.load() % bar;   // first bar line in the buffer
    const float k = float(kWaveW) / float(span);
    const int wy1 = wy + kWaveH;
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
        color(active ? kBg : (content ? kInk : 0x3A4452));
        text((x0 + x1) / 2, (y0 + y1) / 2, kFontSmall, Disp::kOptCenter, kPerfFxName[e]);
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
    static const OptParam kTrackRows[kOptRows] = {OptParam::kStop, OptParam::kLength, OptParam::kCountIn,
                                                  OptParam::kInput, OptParam::kAfter};
    static const OptParam kTempoRows[4] = {OptParam::kTempo, OptParam::kBeats, OptParam::kMetronome,
                                           OptParam::kClick};
    if (row < 0 || row >= kOptRows) return false;
    if (col < kTracks) {
        p = kTrackRows[row];
        return true;
    }
    if (col == 4 && row < 4) {
        p = kTempoRows[row];
        return true;
    }
    return false;
}

void App::optKey(uint8_t id) {
    OptParam p;
    int t;
    if (id == b::F8) {
        options_ = false;
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
        case OptParam::kCountIn: hi = 1; break;
        case OptParam::kInput: hi = 3; break;
        case OptParam::kAfter: hi = 2; break;
        case OptParam::kTempo: lo = 40; hi = 240; break;
        case OptParam::kBeats: lo = 1; hi = 12; break;
        case OptParam::kMetronome: hi = 1; break;
        case OptParam::kClick: hi = 100; step = shift_ ? 1 : 5; break;
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
    static const char* const kHelp[9][4] = {
        {"Stop", "Free: the take ends when you", "press. On bar / on beat: it", "records on to the next line."},
        {"Length", "Recording stops by itself.", "Encoder: whole bars,", "Shift + encoder: beats."},
        {"Count-in", "One bar of click, then the", "take starts on the bar line.", "Press again to cancel."},
        {"Input", "Mono takes half the memory,", "so twice the time. Change it", "while the track is empty."},
        {"After rec", "What the track does when", "the take ends: play, overdub", "or stop."},
        {"Tempo", "Sets the bar until there is a", "loop; after that it follows", "the loop's bar."},
        {"Beats / bar", "The click, on-beat stops and", "the beat lines on the", "waveforms."},
        {"Metronome", "Click on every beat while the", "transport runs. The count-in", "always clicks."},
        {"Click", "Click level.", "", ""},
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
    static const char* const kRowName[kOptRows] = {"Stop", "Length", "Count-in", "Input", "After rec"};
    static const char* const kTempoRow[4] = {"Tempo", "Beats/bar", "Metronome", "Click"};
    const int x0 = c * kKeyW + 4, x1 = (c + 1) * kKeyW - 4, cx = c * kKeyW + kKeyW / 2;
    const uint32_t sc = c < kTracks ? kTrackColor[c] : kAmber;
    const bool sel = optCol_ == c;
    const bool empty = c < kTracks && (TrackState(engine_.view(c).state.load()) == TrackState::kEmpty ||
                                       TrackState(engine_.view(c).state.load()) == TrackState::kClearing);
    const bool haveLoop = engine_.masterLen() != 0;
    char buf[20];

    color(sel ? kLaneSel : kLane);
    fillRect(x0, 6, x1, 428, 8);
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
        const int by0 = 46 + row * 76, by1 = by0 + 68;
        if (sel && optRow_ == row) {
            color(sc);
            fillRect(x0 + 2, by0 - 2, x1 - 2, by1 + 2, 8);
        }
        color(kCell);
        fillRect(x0 + 4, by0, x1 - 4, by1, 6);
        const uint8_t v = engine_.opt(p, t);
        bool locked = false;
        const char* note = nullptr;
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
            case OptParam::kCountIn: std::snprintf(buf, sizeof buf, "%s", v ? "1 bar" : "Off"); break;
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
            case OptParam::kTempo:
                std::snprintf(buf, sizeof buf, "%u bpm", unsigned(v));
                locked = haveLoop;
                note = locked ? "from loop" : nullptr;
                break;
            case OptParam::kBeats: std::snprintf(buf, sizeof buf, "%u", unsigned(v)); break;
            case OptParam::kMetronome: std::snprintf(buf, sizeof buf, "%s", v ? "On" : "Off"); break;
            case OptParam::kClick: std::snprintf(buf, sizeof buf, "%u", unsigned(v)); break;
        }
        color(kDim);
        text(cx, by0 + 14, kFontSmall, Disp::kOptCenter, c < kTracks ? kRowName[row] : kTempoRow[row]);
        color(locked ? kDim : kInk);
        text(cx, by0 + 36, kFontSmall, Disp::kOptCenter, buf);
        if (note) {
            color(0x4A5563);
            text(cx, by0 + 55, kFontSmall, Disp::kOptCenter, note);
        }
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
