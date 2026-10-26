// Tracker Looper simulator: the looper app on the OpenTracker host HAL
// (software FT81x, SDL audio, the front panel drawn around the screen).
//
//   looper-sim [--line-in] [--tone HZ] [--tone-ch left|right|both] [--scale N] [--quit-after MS]
//   looper-sim --script FILE [--tone HZ] [--wav OUT.wav] [--shot OUT.png]   (headless)
//
// Interactive keys: pads = the four keyboard rows 1..=, q..], a..', z../
// (12 columns each, row = track); Enter = Rec, Space = Play, Backspace = Delete,
// Up/Down, F1..F8, Left Shift = Shift, mouse wheel or [ ] = encoder; the panel
// is clickable. Ctrl+Q quits.
//
// Script commands (one per line, # comments):
//   wait MS            advance virtual time
//   tap PAD            pad down, 20 ms, up        down PAD / up PAD
//   press NAME         button down, 30 ms, up     hold NAME / release NAME
//   enc N              encoder detents
//   tone HZ | tone off synthetic line input (bursts of a sine)
//   tone_ch left|right|both   which input channels carry the tone
//   levels             print and reset the output peaks since the last reset
//   reset_levels       reset them without printing
//   expect_audio MIN   fail unless the output peak since the last reset >= MIN (0..1), then reset
//   expect_silent      fail unless the output peak since the last reset < 0.001, then reset
//   expect_ch left|right MIN|silent   the same for one channel (no reset)
//   expect_len TRACK SEC          track (1..4) loop length, within 10 ms
//   expect_state TRACK NAME       EMPTY, COUNT, ARMED, REC, PLAY, DUB or STOP
//   opt TRACK|all NAME VALUE      set an option (stop, length, start, input, after, tempo,
//                                 beats, metronome, click, threshold) as the options page does
//   expect_fx SLOT TYPE [K V]     FX slot (1..2) holds TYPE (and knob K, 1..4, is V)
//   expect_perfknob FX VALUE      performance effect FX (1..8) knob value
//   tone_shape steady|bursts      a continuous sine, or the default bursts
//   expect_below MAX              fail unless the output peak since the last reset < MAX, then reset
//   expect_perf TRACK FX on|off   performance effect FX (1..8, pad columns 5..12) running
//   shot PATH          PNG of the front panel; screen PATH = the 800x480 screen only
#include <SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "canvas.h"
#include "host_hal.h"
#include "looper/app.h"
#include "panel.h"
#include "png.h"
#include "sdl_audio.h"
#include "sw_display.h"
#include "ui/buttons.h"

using namespace sim;
namespace b = ot::btn;

namespace {

struct Options {
    bool lineIn = false;
    float tone = 0.0f;
    int scale = 1;
    long quitAfterMs = -1;
    std::string toneCh = "both";
    bool telemetry = false;   // print the app's once-a-second telemetry line
    std::string script, wav, shot;
};

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--line-in") o.lineIn = true;
        else if (a == "--tone" && (v = next())) o.tone = float(std::atof(v));
        else if (a == "--tone-ch" && (v = next())) o.toneCh = v;
        else if (a == "--telemetry") o.telemetry = true;
        else if (a == "--scale" && (v = next())) o.scale = std::atoi(v) < 1 ? 1 : std::atoi(v);
        else if (a == "--quit-after" && (v = next())) o.quitAfterMs = std::atol(v);
        else if (a == "--script" && (v = next())) o.script = v;
        else if (a == "--wav" && (v = next())) o.wav = v;
        else if (a == "--shot" && (v = next())) o.shot = v;
        else {
            std::fprintf(stderr, "usage: %s [--line-in] [--tone HZ] [--tone-ch left|right|both] [--scale N] [--quit-after MS] "
                                 "[--script FILE [--wav OUT] [--shot OUT.png]]\n", argv[0]);
            return false;
        }
    }
    return true;
}

// Synthetic line input: 0.25 s falling bursts of a sine, every 0.5 s, on both
// channels or only one (stereo checks).
struct ToneIn {
    enum Channels : uint8_t { kBoth, kLeft, kRight };
    float hz = 0.0f;
    Channels ch = kBoth;
    bool steady = false;   // a continuous sine instead of bursts
    uint64_t n = 0;
    int16_t buf[128 * 2] = {};
    const int16_t* block() {
        if (hz <= 0.0f) return nullptr;
        for (int i = 0; i < 128; ++i, ++n) {
            const double t = double(n) / 44100.0;
            const double ph = std::fmod(t, 0.5);
            const double env = steady ? 1.0 : (ph > 0.25 ? 0.0 : (1.0 - ph / 0.25));
            const int16_t v = int16_t(14000.0 * env * std::sin(2.0 * 3.14159265 * hz * t));
            buf[2 * i] = ch == kRight ? 0 : v;
            buf[2 * i + 1] = ch == kLeft ? 0 : v;
        }
        return buf;
    }
};

// Sits between the app and ManualAudio to measure each output channel.
class ChannelProbe final : public ot::IAudioOut {
public:
    explicit ChannelProbe(ManualAudio& a) : a_(a) {}
    void setCallback(FillFn fn, void* ctx) override {
        fn_ = fn;
        ctx_ = ctx;
        a_.setCallback(&ChannelProbe::fill, this);
    }
    bool start() override { return a_.start(); }
    void stop() override { a_.stop(); }
    float peak(int ch) const { return peak_[ch]; }
    void reset() { peak_[0] = peak_[1] = 0.0f; }

private:
    static void fill(int16_t* out, uint32_t frames, void* ctx) {
        ChannelProbe* p = static_cast<ChannelProbe*>(ctx);
        if (p->fn_) p->fn_(out, frames, p->ctx_);
        for (uint32_t i = 0; i < frames * 2; ++i) {
            const float v = std::fabs(float(out[i]) / 32768.0f);
            if (v > p->peak_[i & 1]) p->peak_[i & 1] = v;
        }
    }
    ManualAudio& a_;
    FillFn fn_ = nullptr;
    void* ctx_ = nullptr;
    float peak_[2] = {0.0f, 0.0f};
};

struct Machine {
    SimClock clock;
    SwDisplay display;
    SimInput input{clock};
    SimLeds leds;
    SdlAudio sdlAudio;
    ManualAudio manualAudio;
    ChannelProbe probe{manualAudio};
    ToneIn tone;
    ot::IAudioOut* audio = nullptr;
    std::vector<uint8_t> loopMem, scratch;
    std::vector<float> fxMem;   // float elements: 4-byte aligned
    looper::App app;

    bool start(const Options& o, bool headless) {
        clock.setVirtual(headless);
        tone.hz = o.tone;
        tone.ch = o.toneCh == "left" ? ToneIn::kLeft : (o.toneCh == "right" ? ToneIn::kRight : ToneIn::kBoth);
        looper::Platform p;
        if (headless) {
            audio = &probe;
            if (!o.wav.empty() && !manualAudio.recordWav(o.wav.c_str()))
                std::fprintf(stderr, "cannot write %s\n", o.wav.c_str());
        } else {
            audio = &sdlAudio;
            sdlAudio.enableLineIn(o.lineIn && o.tone <= 0.0f);
        }
        if (o.lineIn && !headless && o.tone <= 0.0f) {
            p.lineIn = [](void* c) { return static_cast<SdlAudio*>(c)->lineIn(); };
            p.lineInCtx = &sdlAudio;
        } else {
            p.lineIn = [](void* c) { return static_cast<ToneIn*>(c)->block(); };
            p.lineInCtx = &tone;
        }
        // Same sizes as the device: 16 MB SDRAM minus the effect lines and the 64 KB staging buffer.
        const uint32_t fxBytes = (looper::kFxMemBytes + 15u) & ~15u;
        loopMem.assign(16u * 1024u * 1024u - 64u * 1024u - fxBytes, 0xA5);   // garbage, like SDRAM after power-up
        fxMem.assign(fxBytes / sizeof(float), 1.0f);
        scratch.assign(64u * 1024u, 0);
        p.fxMem = reinterpret_cast<uint8_t*>(fxMem.data());
        p.display = &display;
        p.audio = audio;
        p.input = &input;
        p.leds = &leds;
        p.clock = &clock;
        p.loopMem = loopMem.data();
        p.loopMemBytes = uint32_t(loopMem.size());
        p.scratch = scratch.data();
        p.scratchBytes = uint32_t(scratch.size());
        p.powerOff = [] { std::exit(0); };
        if (o.telemetry) p.log = [](const char* line) { std::printf("%s\n", line); };
        if (!app.init(p)) {
            std::fprintf(stderr, "looper init failed\n");
            return false;
        }
        return true;
    }
};

void panelShot(Machine& m, const std::string& path, bool screenOnly) {
    std::vector<uint32_t> fb(800 * 480);
    m.display.frontCopy(fb.data());
    bool ok;
    if (screenOnly) {
        ok = writePng(path.c_str(), fb.data(), 800, 480);
    } else {
        Canvas c(Panel::kW, Panel::kH);
        Panel().draw(c, fb.data(), m.input, m.leds, 0, "Tracker Looper (headless)");
        ok = writePng(path.c_str(), c.pixels(), Panel::kW, Panel::kH);
    }
    std::printf("%s %s\n", ok ? "saved" : "FAILED to save", path.c_str());
}

// ---------------------------------------------------------------------------
// Headless: virtual time, audio pulled block by block, app.loop() every ms.

struct Headless {
    Machine& m;
    double frameAcc = 0.0;

    void resetLevels() {
        m.manualAudio.resetLevels();
        m.probe.reset();
    }

    void advance(uint32_t ms) {
        for (uint32_t i = 0; i < ms; ++i) {
            m.clock.advanceUs(1000);
            frameAcc += 44.1;
            while (frameAcc >= 128.0) {
                m.manualAudio.pull();
                frameAcc -= 128.0;
            }
            m.app.loop();
        }
    }

    bool run(const std::string& path) {
        std::ifstream f(path);
        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", path.c_str());
            return false;
        }
        std::string line;
        int lineNo = 0;
        bool ok = true;
        while (std::getline(f, line)) {
            ++lineNo;
            const size_t hash = line.find('#');
            if (hash != std::string::npos) line.erase(hash);
            std::istringstream in(line);
            std::string cmd, arg;
            if (!(in >> cmd)) continue;
            in >> arg;
            auto btn = [&](const std::string& n) {
                const int id = buttonByName(n.c_str());
                if (id < 0) std::fprintf(stderr, "%s:%d: unknown button %s\n", path.c_str(), lineNo, n.c_str());
                return id;
            };
            if (cmd == "wait") advance(uint32_t(std::atol(arg.c_str())));
            else if (cmd == "tap") {
                m.input.pad(uint8_t(std::atoi(arg.c_str())), true, 100);
                advance(20);
                m.input.pad(uint8_t(std::atoi(arg.c_str())), false);
            } else if (cmd == "down") m.input.pad(uint8_t(std::atoi(arg.c_str())), true, 100);
            else if (cmd == "up") m.input.pad(uint8_t(std::atoi(arg.c_str())), false);
            else if (cmd == "press") {
                const int id = btn(arg);
                if (id >= 0) {
                    m.input.button(uint8_t(id), true);
                    advance(30);
                    m.input.button(uint8_t(id), false);
                }
            } else if (cmd == "hold" || cmd == "release") {
                const int id = btn(arg);
                if (id >= 0) m.input.button(uint8_t(id), cmd == "hold");
            } else if (cmd == "enc") m.input.encoder(std::atoi(arg.c_str()));
            else if (cmd == "tone") m.tone.hz = arg == "off" ? 0.0f : float(std::atof(arg.c_str()));
            else if (cmd == "tone_ch") {
                if (arg == "left") m.tone.ch = ToneIn::kLeft;
                else if (arg == "right") m.tone.ch = ToneIn::kRight;
                else m.tone.ch = ToneIn::kBoth;
            } else if (cmd == "levels") {
                std::printf("levels: peak %.3f rms %.3f (L %.3f R %.3f)\n", m.manualAudio.peakLevel(),
                            m.manualAudio.rmsLevel(), m.probe.peak(0), m.probe.peak(1));
                resetLevels();
            } else if (cmd == "expect_audio" || cmd == "expect_silent") {
                const float pk = m.manualAudio.peakLevel();
                const bool pass = cmd == "expect_silent" ? pk < 0.001f : pk >= float(std::atof(arg.c_str()));
                std::printf("%s:%d: %s %s (peak %.3f)\n", path.c_str(), lineNo, cmd.c_str(), pass ? "PASS" : "FAIL", pk);
                ok = ok && pass;
                resetLevels();
            } else if (cmd == "expect_ch") {
                // expect_ch left|right MIN   or   expect_ch left|right silent
                std::string lim;
                in >> lim;
                const int ch = arg == "right" ? 1 : 0;
                const float pk = m.probe.peak(ch);
                const bool pass = lim == "silent" ? pk < 0.001f : pk >= float(std::atof(lim.c_str()));
                std::printf("%s:%d: expect_ch %s %s %s (peak %.3f)\n", path.c_str(), lineNo, arg.c_str(), lim.c_str(),
                            pass ? "PASS" : "FAIL", pk);
                ok = ok && pass;
            } else if (cmd == "expect_len") {
                // expect_len TRACK SECONDS: the loop length, to within 10 ms
                std::string sec;
                in >> sec;
                const int t = std::atoi(arg.c_str()) - 1;
                const double got = t >= 0 && t < looper::kTracks
                                       ? double(m.app.engine().view(t).len.load()) / 44100.0 : -1.0;
                const bool pass = std::fabs(got - std::atof(sec.c_str())) < 0.01;
                std::printf("%s:%d: expect_len %s %s %s (%.3f s)\n", path.c_str(), lineNo, arg.c_str(), sec.c_str(),
                            pass ? "PASS" : "FAIL", got);
                ok = ok && pass;
            } else if (cmd == "expect_state") {
                // expect_state TRACK EMPTY|COUNT|REC|PLAY|DUB|STOP
                static const char* const kNames[] = {"CLEAR", "EMPTY", "COUNT", "ARMED", "REC", "PLAY", "DUB", "STOP"};
                std::string want;
                in >> want;
                const int t = std::atoi(arg.c_str()) - 1;
                const int s = t >= 0 && t < looper::kTracks ? int(m.app.engine().view(t).state.load()) : 0;
                const bool pass = s >= 0 && s < 8 && want == kNames[s];
                std::printf("%s:%d: expect_state %s %s %s (%s)\n", path.c_str(), lineNo, arg.c_str(), want.c_str(),
                            pass ? "PASS" : "FAIL", s >= 0 && s < 8 ? kNames[s] : "?");
                ok = ok && pass;
            } else if (cmd == "opt") {
                // opt TRACK|all NAME VALUE: a track option, as the options page sets it
                static const char* const kNames[] = {"stop",  "length",    "start", "input", "after",
                                                     "tempo", "beats", "metronome", "click", "threshold"};
                std::string name, val;
                in >> name >> val;
                int p = -1;
                for (int i = 0; i < 10; ++i)
                    if (name == kNames[i]) p = i;
                if (p < 0) {
                    std::fprintf(stderr, "%s:%d: unknown option %s\n", path.c_str(), lineNo, name.c_str());
                    ok = false;
                } else {
                    const int v = std::atoi(val.c_str());
                    const int t0 = arg == "all" ? 0 : std::atoi(arg.c_str()) - 1;
                    const int t1 = arg == "all" ? looper::kTracks - 1 : t0;
                    for (int t = t0; t <= t1; ++t) m.app.scriptOpt(t, looper::OptParam(p), v);
                    advance(5);
                }
            } else if (cmd == "expect_fx") {
                // expect_fx SLOT TYPE [KNOB VALUE]: slot 1..2, type name as on the screen
                std::string type, kn, kv;
                in >> type >> kn >> kv;
                const int s = std::atoi(arg.c_str()) - 1;
                bool pass = s >= 0 && s < looper::kFxSlots &&
                            type == looper::fxTypeInfo(m.app.engine().fxType(s)).name;
                if (pass && !kn.empty())
                    pass = m.app.engine().fxKnob(s, std::atoi(kn.c_str()) - 1) == std::atoi(kv.c_str());
                std::printf("%s:%d: expect_fx %s %s %s %s %s (%s, knobs %u %u %u %u)\n", path.c_str(), lineNo,
                            arg.c_str(), type.c_str(), kn.c_str(), kv.c_str(), pass ? "PASS" : "FAIL",
                            s >= 0 && s < looper::kFxSlots ? looper::fxTypeInfo(m.app.engine().fxType(s)).name : "?",
                            unsigned(m.app.engine().fxKnob(s, 0)), unsigned(m.app.engine().fxKnob(s, 1)),
                            unsigned(m.app.engine().fxKnob(s, 2)), unsigned(m.app.engine().fxKnob(s, 3)));
                ok = ok && pass;
            } else if (cmd == "expect_perfknob") {
                // expect_perfknob FX VALUE: performance effect FX (1..8) knob value
                std::string val;
                in >> val;
                const int e = std::atoi(arg.c_str()) - 1;
                const int got = e >= 0 && e < looper::kPerfFxCount ? m.app.engine().perfParam(looper::PerfFx(e)) : -1;
                const bool pass = got == std::atoi(val.c_str());
                std::printf("%s:%d: expect_perfknob %s %s %s (%d)\n", path.c_str(), lineNo, arg.c_str(), val.c_str(),
                            pass ? "PASS" : "FAIL", got);
                ok = ok && pass;
            } else if (cmd == "tone_shape") m.tone.steady = arg == "steady";
            else if (cmd == "expect_below") {
                const float pk = m.manualAudio.peakLevel();
                const bool pass = pk < float(std::atof(arg.c_str()));
                std::printf("%s:%d: expect_below %s %s (peak %.3f)\n", path.c_str(), lineNo, arg.c_str(),
                            pass ? "PASS" : "FAIL", pk);
                ok = ok && pass;
                resetLevels();
            } else if (cmd == "expect_perf") {
                // expect_perf TRACK FX on|off: FX 1..8 as on the pads (columns 5..12)
                std::string fx, want;
                in >> fx >> want;
                const int t = std::atoi(arg.c_str()) - 1, e = std::atoi(fx.c_str()) - 1;
                const uint8_t mask = t >= 0 && t < looper::kTracks ? m.app.engine().view(t).perf.load() : 0;
                const bool isOn = e >= 0 && e < 8 && ((mask >> e) & 1);
                const bool pass = (want == "on") == isOn;
                std::printf("%s:%d: expect_perf %s %s %s %s (mask 0x%02x)\n", path.c_str(), lineNo, arg.c_str(),
                            fx.c_str(), want.c_str(), pass ? "PASS" : "FAIL", unsigned(mask));
                ok = ok && pass;
            } else if (cmd == "reset_levels") resetLevels();
            else if (cmd == "shot" || cmd == "screen") panelShot(m, arg, cmd == "screen");
            else {
                std::fprintf(stderr, "%s:%d: unknown command %s\n", path.c_str(), lineNo, cmd.c_str());
                ok = false;
            }
        }
        return ok;
    }
};

// ---------------------------------------------------------------------------
// Interactive

int keyToButton(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_RETURN: return b::Rec;
        case SDL_SCANCODE_SPACE: return b::Play;
        case SDL_SCANCODE_BACKSPACE: case SDL_SCANCODE_DELETE: return b::Delete;
        case SDL_SCANCODE_UP: return b::Up;
        case SDL_SCANCODE_DOWN: return b::Down;
        case SDL_SCANCODE_LEFT: return b::Left;
        case SDL_SCANCODE_RIGHT: return b::Right;
        case SDL_SCANCODE_LSHIFT: return b::Shift;
        case SDL_SCANCODE_ESCAPE: return b::Power;
        default: break;
    }
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F8) return b::F1 + (sc - SDL_SCANCODE_F1);
    return -1;
}

int keyToPad(SDL_Scancode sc) {
    static const SDL_Scancode rows[4][12] = {
        {SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6,
         SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9, SDL_SCANCODE_0, SDL_SCANCODE_MINUS, SDL_SCANCODE_EQUALS},
        {SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R, SDL_SCANCODE_T, SDL_SCANCODE_Y,
         SDL_SCANCODE_U, SDL_SCANCODE_I, SDL_SCANCODE_O, SDL_SCANCODE_P, SDL_SCANCODE_LEFTBRACKET,
         SDL_SCANCODE_RIGHTBRACKET},
        {SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_H,
         SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L, SDL_SCANCODE_SEMICOLON, SDL_SCANCODE_APOSTROPHE,
         SDL_SCANCODE_BACKSLASH},
        {SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_B, SDL_SCANCODE_N,
         SDL_SCANCODE_M, SDL_SCANCODE_COMMA, SDL_SCANCODE_PERIOD, SDL_SCANCODE_SLASH, SDL_SCANCODE_RSHIFT,
         SDL_SCANCODE_UNKNOWN},
    };
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 12; ++c)
            if (rows[r][c] == sc && sc != SDL_SCANCODE_UNKNOWN) return r * 12 + c;
    return -1;
}

int runInteractive(Machine& m, const Options& o) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("Tracker Looper simulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       Panel::kW * o.scale, Panel::kH * o.scale, SDL_WINDOW_RESIZABLE);
    if (!win) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return 1;
    }
    SDL_RenderSetLogicalSize(ren, Panel::kW, Panel::kH);
    SDL_Texture* tex =
        SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, Panel::kW, Panel::kH);
    Panel panel;
    Canvas canvas(Panel::kW, Panel::kH);
    std::vector<uint32_t> fb(800 * 480);
    Control mouseCtl;
    bool quit = false;
    uint32_t lastPresent = 0;
    const std::string status = std::string("Pads: keyboard rows 1..= q..] a..' z../ (row = track)\n") +
                               "Enter Rec  Space Play  Backspace Delete  Up/Down  F1..F8\n" +
                               "Wheel or [ ] encoder  LShift Shift  Esc Power\n" +
                               "Input: " + (o.tone > 0 ? "synthetic tone" : (o.lineIn ? "host microphone" : "none")) +
                               "\nCtrl+Q quits";
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT: quit = true; break;
                case SDL_KEYDOWN:
                case SDL_KEYUP: {
                    if (ev.key.repeat) break;
                    const bool down = ev.type == SDL_KEYDOWN;
                    const SDL_Scancode sc = ev.key.keysym.scancode;
                    if (down && sc == SDL_SCANCODE_Q && (ev.key.keysym.mod & KMOD_CTRL)) {
                        quit = true;
                        break;
                    }
                    if (sc == SDL_SCANCODE_LEFTBRACKET && (ev.key.keysym.mod & KMOD_CTRL)) break;
                    const int id = keyToButton(sc);
                    if (id >= 0) {
                        m.input.button(uint8_t(id), down);
                        break;
                    }
                    const int p = keyToPad(sc);
                    if (p >= 0) m.input.pad(uint8_t(p), down, 100);
                    break;
                }
                case SDL_MOUSEWHEEL: {
                    int d = ev.wheel.y;
                    if (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) d = -d;
                    if (d) m.input.encoder(d);
                    break;
                }
                case SDL_MOUSEBUTTONDOWN: {
                    if (ev.button.button != SDL_BUTTON_LEFT) break;
                    const Control c = panel.hit(ev.button.x, ev.button.y);
                    mouseCtl = c;
                    switch (c.kind) {
                        case Control::kButton: m.input.button(c.id, true); break;
                        case Control::kPad: m.input.pad(c.id, true, 100); break;
                        case Control::kEncoderCcw: m.input.encoder(-1); break;
                        case Control::kEncoderCw: m.input.encoder(1); break;
                        case Control::kEncoderPush: m.input.button(b::EncPush, true); break;
                        default: break;
                    }
                    break;
                }
                case SDL_MOUSEBUTTONUP: {
                    if (ev.button.button != SDL_BUTTON_LEFT) break;
                    const Control c = mouseCtl;
                    mouseCtl = Control();
                    if (c.kind == Control::kButton) m.input.button(c.id, false);
                    if (c.kind == Control::kPad) m.input.pad(c.id, false);
                    if (c.kind == Control::kEncoderPush) m.input.button(b::EncPush, false);
                    break;
                }
                default: break;
            }
        }
        m.app.loop();
        const uint32_t now = SDL_GetTicks();
        if (o.quitAfterMs >= 0 && long(now) >= o.quitAfterMs) quit = true;
        if (now - lastPresent >= 16) {
            lastPresent = now;
            m.display.frontCopy(fb.data());
            panel.draw(canvas, fb.data(), m.input, m.leds, 0, status);
            SDL_UpdateTexture(tex, nullptr, canvas.pixels(), Panel::kW * 4);
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, nullptr, nullptr);
            SDL_RenderPresent(ren);
        } else {
            SDL_Delay(1);
        }
    }
    if (m.audio) m.audio->stop();
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parseArgs(argc, argv, o)) return 2;
    const bool headless = !o.script.empty();
    static Machine m;   // 16 MB of loop memory: keep it off the stack
    if (!m.start(o, headless)) return 1;
    if (!headless) return runInteractive(m, o);
    Headless h{m};
    h.advance(300);   // boot: zero the loop memory, first frames
    const bool ok = h.run(o.script);
    h.advance(50);
    if (!o.shot.empty()) panelShot(m, o.shot, false);
    if (m.audio) m.audio->stop();
    return ok ? 0 : 1;
}
