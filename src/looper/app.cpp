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
        case TrackState::kRecording: return "REC";
        case TrackState::kPlaying: return "PLAY";
        case TrackState::kOverdubbing: return "DUB";
        case TrackState::kStopped: return "STOP";
    }
    return "";
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
    if (!d_ || !p_.audio || !p_.input || !p_.leds || !p_.clock || !p_.loopMem) return false;
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
    engine_.init(mem);

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
    if (!down) return;
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

void App::onPad(int pad, bool down) {
    if (!down || pad < 0 || pad >= ot::kPadCount) return;
    const int t = pad / ot::kPadCols, col = pad % ot::kPadCols;
    if (t >= kTracks) return;
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
    ot::ILeds& L = *p_.leds;
    const float blink = pulse(now, 600);
    for (int t = 0; t < kTracks; ++t) {
        const TrackView& v = engine_.view(t);
        const TrackState s = TrackState(v.state.load());
        const bool muted = v.muted.load();
        const uint32_t len = v.len.load(), pos = v.pos.load();
        const bool content = len > 0;
        const uint32_t tc = kTrackColor[t];
        const int row = t * ot::kPadCols;
        auto set = [&](int col, uint32_t c) { L.setPad(uint8_t(row + col), r8(c), g8(c), b8(c)); };

        uint32_t rec = scale(kRed, 0.12f);
        if (s == TrackState::kRecording) rec = scale(kRed, 0.4f + 0.6f * blink);
        else if (s == TrackState::kOverdubbing) rec = scale(kAmber, 0.4f + 0.6f * blink);
        set(0, rec);
        set(1, s == TrackState::kPlaying || s == TrackState::kOverdubbing ? 0x00C060
               : content ? 0x003018 : 0);
        set(2, muted ? 0xC0A000 : (content ? 0x201A00 : 0));
        set(3, content || s == TrackState::kRecording ? 0x202020 : 0);
        const int seg = content ? int(uint64_t(pos) * kSegments / len) : -1;
        for (int c = 0; c < kSegments; ++c) {
            uint32_t col = 0;
            if (content) col = c == seg ? tc : scale(tc, muted ? 0.03f : 0.08f);
            set(4 + c, col);
        }
    }
    L.show();
}

// ---------------------------------------------------------------------------
// Waveform bitmaps (RGB332 intensity, tinted by COLOR_RGB at draw time)

void App::renderWave(int t, uint32_t span) {
    const TrackMemory& m = engine_.memory(t);
    const uint32_t written = engine_.view(t).written.load();
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
    const uint8_t baseline = uint8_t((1 << 5) | (1 << 2));
    for (int x = 0; x < kWaveW; ++x) {
        const uint32_t c0 = uint32_t(uint64_t(span) * x / kWaveW / kPeakFrames);
        uint32_t c1 = uint32_t(uint64_t(span) * (x + 1) / kWaveW / kPeakFrames);
        if (c1 <= c0) c1 = c0 + 1;
        uint8_t pl = 0, pr = 0;
        for (uint32_t c = c0; c < c1 && c < cells; ++c) {
            if (m.peaks[2 * c] > pl) pl = m.peaks[2 * c];
            if (m.peaks[2 * c + 1] > pr) pr = m.peaks[2 * c + 1];
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
    const uint32_t L = engine_.masterLen();
    for (int t = 0; t < kTracks; ++t) {
        const TrackView& v = engine_.view(t);
        const TrackState s = TrackState(v.state.load());
        if (s == TrackState::kEmpty || s == TrackState::kClearing) {
            waveValid_[t] = false;
            continue;
        }
        uint32_t span = v.len.load();
        if (s == TrackState::kRecording) {
            const uint32_t w = v.written.load();
            if (L) {
                span = L;
                while (span < w) span += L;
            } else {
                span = kSampleRate * 2;   // the master take: double the view as it grows
                while (span < w) span *= 2;
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
    color(mode == PlayMode::kForward ? kDim : tc);
    text(16, y0 + 44, kFontSmall, 0, modeInfo(mode).name);
    if (muted) {
        color(0xC0A000);
        text(120, y0 + 44, kFontSmall, Disp::kOptRightX, "MUTE");
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
    if (s == TrackState::kRecording) {
        color(kRed);
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

    if (empty) {
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

        // Bar grid: master-length divisions, or eighths when the track is one bar.
        const uint32_t L = engine_.masterLen();
        if (L && span > L) {
            color(0x3A4452);
            for (uint32_t f = L; f < span; f += L) vline(wx + int(uint64_t(kWaveW) * f / span), wy, wy1, 16);
        } else {
            color(0x1E252F);
            for (int i = 1; i < kSegments; ++i) vline(wx + kWaveW * i / kSegments, wy, wy1, 16);
        }
        color(kDim);
        text(wx + 5, wy + 2, kFontSmall, 0, "L");
        text(wx + 5, wy1 - 16, kFontSmall, 0, "R");

        // Playhead with a glow
        const uint32_t head = s == TrackState::kRecording ? kRed : (s == TrackState::kOverdubbing ? kAmber : kInk);
        color(head);
        alpha(60);
        vline(px, wy - 1, wy1 + 1, 96);
        alpha(255);
        vline(px, wy - 1, wy1 + 1, 24);
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
    for (int i = 0; i < 8; ++i) {
        const int x0 = i * kKeyW + 3, x1 = (i + 1) * kKeyW - 3, cx = i * kKeyW + kKeyW / 2;
        bool hot = false;
        if (modeBar_) {
            title[i] = "";
            std::snprintf(val[i], sizeof val[i], "%s", kModes[i].name);
            hot = int(mode) == i;
        } else {
            switch (i) {
                case 0:
                    title[i] = "REC";
                    std::snprintf(val[i], sizeof val[i], "%s",
                                  s == TrackState::kEmpty ? "Record"
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
        color(hot ? tc : kCell);
        fillRect(x0, kKeyY0, x1, kKeyY1, 6);
        if (title[i][0]) {
            color(hot ? kBg : (!modeBar_ && i < 5 ? scale(tc, 0.9f) : kDim));
            text(cx, kKeyY0 + 10, kFontSmall, Disp::kOptCenter, title[i]);
        }
        color(hot ? kBg : kInk);
        text(cx, modeBar_ ? (kKeyY0 + kKeyY1) / 2 + 4 : kKeyY0 + 28, kFontSmall,
             Disp::kOptCenter, val[i]);
    }
    // Input level inside the INPUT cell
    if (!modeBar_) {
        const int x0 = 5 * kKeyW + 10, x1 = 6 * kKeyW - 10;
        for (int c = 0; c < 2; ++c) {
            const float m = inMeter_[c];
            if (m < 0.005f) continue;
            color(m > 0.9f ? kRed : (m > 0.6f ? kAmber : 0x3DDC97));
            hline(x0, x0 + int((x1 - x0) * m), kKeyY1 - 5 + c * 2, 16);
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
