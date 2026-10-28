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
//   expect_state TRACK NAME       EMPTY, COUNT, ARMED, REC, PLAY, DUB, STOP or QUEUED
//   expect_opt also takes monitor, feedback, gain (the Input page's other settings) and volume
//   opt TRACK|all NAME VALUE      set an option (stop, length, start, input, after, tempo,
//                                 beats, metronome, click, threshold) as the options page does
//   expect_opt TRACK NAME VALUE   an option's value (names as for opt)
//   expect_file PATH MINBYTES     a file on the SD card folder, at least MINBYTES long
//   sd_file PATH BYTES            write a file of BYTES zero bytes on the SD card folder
//   expect_flash NAME|none        the image the last firmware update asked for
//   expect_pitch HZ [TOL]         the left output's frequency since reset_levels, within 3 % (or TOL %)
//   expect_mode TRACK NAME        playback mode (label without spaces: Halfspeed, Granular)
//   midi_in HEX...                bytes in on DIN;  midi_clock BPM [start|stop]  a clock in (0: none)
//   reset_midi / expect_midi_out din|usb BYTE MIN [MAX]   count a byte sent since reset_midi
//   mix TRACK NAME VALUE          a mixer setting (level send1 send2 return1 return2 master chain1 chain2)
//   expect_smooth MAX             no left-output step between two frames above MAX since reset_levels (clicks)
//   expect_grain TRACK KNOB VALUE Granular mode's knob 1..4 (spread, size, density, pitch)
//   expect_fx SLOT TYPE [K V]     FX slot (1..2) holds TYPE (and knob K, 1..4, is V)
//   expect_perfknob FX VALUE      performance effect FX (1..8) knob value
//   tone_shape steady|bursts      a continuous sine, or the default bursts
//   expect_stream TRACK long 0|1  the track holds a long take (its tail on the card)
//   expect_stream TRACK under|over MAX   frames played as silence / takes cut short, at most MAX
//   reset_stream TRACK            zero those counts
//   card_speed WRITE_MBS READ_MBS LATENCY_MS [STALL_MS EVERY]   card commands take time (the
//                                 audio runs on, as on the device): a fixed latency, the MB/s,
//                                 and every EVERY-th write a stall; card_speed 0: instant
//   input_wav PATH | input_wav off  a 16-bit 44.1 kHz WAV (mono or stereo) as the line
//                                 input, from its start, looping (in place of the tone)
//   expect_below MAX              fail unless the output peak since the last reset < MAX, then reset
//   expect_perf TRACK FX on|off   performance effect FX (1..8, pad columns 5..12) running
//   shot PATH          PNG of the front panel; screen PATH = the 800x480 screen only
#include <random>
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
#include "host_storage.h"
#include "looper/app.h"
#include "panel.h"
#include "png.h"
#include "sdl_audio.h"
#include "sw_display.h"
#include "ui/buttons.h"

using namespace sim;
namespace b = ot::btn;

namespace {

std::string gFlashed;   // the image a firmware update asked for (expect_flash)

struct Options {
    bool lineIn = false;
    float tone = 0.0f;
    int scale = 1;
    long quitAfterMs = -1;
    std::string toneCh = "both";
    bool telemetry = false;   // print the app's once-a-second telemetry line
    std::string sd;           // the SD card: a host folder (default: "sd" next to the program)
    std::string script, wav, shot;
    long soakSeed = -1;       // --soak SEED: a long random session instead of a script
    long soakMinutes = 60;
    std::string soakLog;      // every action, timed (default: none)
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
        else if (a == "--sd" && (v = next())) o.sd = v;
        else if (a == "--scale" && (v = next())) o.scale = std::atoi(v) < 1 ? 1 : std::atoi(v);
        else if (a == "--quit-after" && (v = next())) o.quitAfterMs = std::atol(v);
        else if (a == "--script" && (v = next())) o.script = v;
        else if (a == "--wav" && (v = next())) o.wav = v;
        else if (a == "--shot" && (v = next())) o.shot = v;
        else if (a == "--soak" && (v = next())) o.soakSeed = std::atol(v);
        else if (a == "--minutes" && (v = next())) o.soakMinutes = std::atol(v);
        else if (a == "--soak-log" && (v = next())) o.soakLog = v;
        else {
            std::fprintf(stderr, "usage: %s [--line-in] [--tone HZ] [--tone-ch left|right|both] [--scale N] [--quit-after MS] "
                                 "[--script FILE [--wav OUT] [--shot OUT.png]] [--soak SEED [--minutes N] [--soak-log FILE]]\n",
                         argv[0]);
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
    std::vector<int16_t> wav;   // input_wav: stereo frames, looped
    size_t wavPos = 0;
    // loopback: the output back into the input, loopDelay frames later (a
    // cable from out to in, with the codec's round trip).
    uint32_t loopDelay = 0;
    float loopGain = 0.0f;
    std::vector<int16_t> loopBuf = std::vector<int16_t>(2 * 8192);
    uint64_t outFrames = 0, inFrames = 0;
    void pushOut(const int16_t* out, uint32_t frames) {
        for (uint32_t i = 0; i < frames; ++i, ++outFrames) {
            const size_t j = 2 * (outFrames % 8192);
            loopBuf[j] = out[2 * i];
            loopBuf[j + 1] = out[2 * i + 1];
        }
    }
    const int16_t* withLoop(const int16_t* src) {
        // src (or silence) plus the output loopDelay frames ago.
        const uint64_t base = inFrames;
        inFrames += 128;
        if (!loopDelay) return src;
        for (int i = 0; i < 128; ++i) {
            int32_t l = src ? src[2 * i] : 0, r = src ? src[2 * i + 1] : 0;
            const uint64_t at = base + uint64_t(i);
            if (at >= loopDelay && at - loopDelay < outFrames && outFrames - (at - loopDelay) <= 8192) {
                const size_t j = 2 * ((at - loopDelay) % 8192);
                l += int32_t(float(loopBuf[j]) * loopGain);
                r += int32_t(float(loopBuf[j + 1]) * loopGain);
            }
            buf[2 * i] = int16_t(l < -32768 ? -32768 : (l > 32767 ? 32767 : l));
            buf[2 * i + 1] = int16_t(r < -32768 ? -32768 : (r > 32767 ? 32767 : r));
        }
        return buf;
    }
    bool loadWav(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0) return false;
        int chans = 0, bits = 0;
        for (size_t p = 12; p + 8 <= d.size();) {
            uint32_t len;
            std::memcpy(&len, &d[p + 4], 4);
            if (std::memcmp(&d[p], "fmt ", 4) == 0 && len >= 16) {
                uint16_t c, bps;
                std::memcpy(&c, &d[p + 10], 2);
                std::memcpy(&bps, &d[p + 22], 2);
                chans = c;
                bits = bps;
            } else if (std::memcmp(&d[p], "data", 4) == 0 && bits == 16 && (chans == 1 || chans == 2)) {
                const size_t frames = std::min<size_t>(len, d.size() - p - 8) / size_t(2 * chans);
                wav.assign(frames * 2, 0);
                const int16_t* s = reinterpret_cast<const int16_t*>(&d[p + 8]);
                for (size_t i = 0; i < frames; ++i) {
                    wav[2 * i] = s[i * chans];
                    wav[2 * i + 1] = s[i * chans + chans - 1];
                }
                wavPos = 0;
                return frames > 0;
            }
            p += 8 + len + (len & 1);
        }
        return false;
    }
    const int16_t* block() { return withLoop(source()); }
    const int16_t* source() {
        if (!wav.empty()) {
            for (int i = 0; i < 128; ++i) {
                buf[2 * i] = wav[wavPos];
                buf[2 * i + 1] = wav[wavPos + 1];
                wavPos = (wavPos + 2) % wav.size();
            }
            return buf;
        }
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
    ToneIn* loop = nullptr;   // gets every output block (loopback)
    // Onsets on the left output since reset: |x| over 0.05 after 300 quiet frames.
    uint32_t onsets() const { return onsets_; }
    void setCallback(FillFn fn, void* ctx) override {
        fn_ = fn;
        ctx_ = ctx;
        a_.setCallback(&ChannelProbe::fill, this);
    }
    bool start() override { return a_.start(); }
    void stop() override { a_.stop(); }
    float peak(int ch) const { return peak_[ch]; }
    void reset() {
        peak_[0] = peak_[1] = 0.0f;
        rises_ = frames_ = 0;
        maxStep_ = 0.0f;
        onsets_ = 0;
    }
    float maxStep() const { return maxStep_; }   // largest |x[n] - x[n-1]|, left, since reset
    // The left output's frequency since the last reset: rising crossings
    // through +-2 % (hysteresis, so grain fades do not count) over the time.
    float pitchHz() const { return frames_ ? float(rises_) * 44100.0f / float(frames_) : 0.0f; }

private:
    static void fill(int16_t* out, uint32_t frames, void* ctx) {
        ChannelProbe* p = static_cast<ChannelProbe*>(ctx);
        if (p->fn_) p->fn_(out, frames, p->ctx_);
        if (p->loop) p->loop->pushOut(out, frames);
        for (uint32_t f = 0; f < frames; ++f) {
            if (std::fabs(float(out[2 * f]) / 32768.0f) > 0.05f) {
                if (p->quiet_ >= 300) ++p->onsets_;
                p->quiet_ = 0;
            } else {
                ++p->quiet_;
            }
        }
        for (uint32_t i = 0; i < frames * 2; ++i) {
            const float v = std::fabs(float(out[i]) / 32768.0f);
            if (v > p->peak_[i & 1]) p->peak_[i & 1] = v;
        }
        for (uint32_t f = 0; f < frames; ++f) {
            const float l = float(out[2 * f]) / 32768.0f;
            const float st = std::fabs(l - p->last_);
            if (st > p->maxStep_) p->maxStep_ = st;
            p->last_ = l;
            if (!p->high_ && l > 0.02f) {
                p->high_ = true;
                ++p->rises_;
            } else if (p->high_ && l < -0.02f) {
                p->high_ = false;
            }
        }
        p->frames_ += frames;
    }
    ManualAudio& a_;
    FillFn fn_ = nullptr;
    void* ctx_ = nullptr;
    float peak_[2] = {0.0f, 0.0f};
    uint32_t rises_ = 0, frames_ = 0;
    bool high_ = false;
    float last_ = 0.0f, maxStep_ = 0.0f;
    uint32_t onsets_ = 0, quiet_ = 1000;
};

// MIDI: script bytes in (DIN), everything sent kept for expect_midi_out.
class SimMidi final : public ot::IMidiPort {
public:
    std::vector<uint8_t> in;
    size_t inPos = 0;
    std::vector<uint8_t> out[2];
    bool send(ot::MidiPortId port, const uint8_t* data, uint32_t len) override {
        out[int(port) & 1].insert(out[int(port) & 1].end(), data, data + len);
        return true;
    }
    uint32_t receive(ot::MidiPortId port, uint8_t* dst, uint32_t cap) override {
        if (port != ot::MidiPortId::kDin) return 0;
        uint32_t n = 0;
        while (n < cap && inPos < in.size()) dst[n++] = in[inPos++];
        if (inPos == in.size()) {
            in.clear();
            inPos = 0;
        }
        return n;
    }
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
    HostStorage storage;
    SimMidi midi;
    double clockBpm = 0.0, nextTickMs = 0.0;   // midi_clock: a clock coming in
    ot::IAudioOut* audio = nullptr;
    std::vector<uint8_t> loopMem, scratch;
    std::vector<float> fxMem;   // float elements: 4-byte aligned
    looper::App app;

    bool start(const Options& o, bool headless) {
        clock.setVirtual(headless);
        probe.loop = &tone;   // the loopback hears every output block
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
        if (!o.sd.empty() && storage.init(o.sd)) {
            p.storage = &storage;
            p.storageReady = [] { return true; };
        } else {
            std::fprintf(stderr, "no SD folder: settings and projects are off\n");
        }
        p.midi = &midi;
        p.version = "looper-0.1";   // as the device build, so the Firmware page can mark its image
        // The simulator has no bootloader: it notes the image and carries on.
        p.firmwareUpdate = [](const char* name) {
            gFlashed = name;
            std::printf("firmware update requested: %s\n", name);
            return true;
        };
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
    // card_speed: how long a card command takes.
    double wMBs = 0.0, rMBs = 0.0, latMs = 0.0, stallMs = 0.0;
    uint32_t stallEvery = 0, writes = 0;
    bool inIo = false;

    // The main loop is blocked in a card command: the audio interrupt runs on.
    void stall(double us) {
        m.clock.advanceUs(uint64_t(us));
        frameAcc += us * 44.1 / 1000.0;
        while (frameAcc >= 128.0) {
            m.manualAudio.pull();
            frameAcc -= 128.0;
        }
    }
    static void ioHook(uint32_t bytes, bool write, void* ctx) {
        Headless& h = *static_cast<Headless*>(ctx);
        if (h.inIo) return;
        h.inIo = true;
        double ms = h.latMs + double(bytes) / ((write ? h.wMBs : h.rMBs) * 1048.576);
        if (write && h.stallEvery && ++h.writes % h.stallEvery == 0) ms += h.stallMs;
        h.stall(ms * 1000.0);
        h.inIo = false;
    }

    void resetLevels() {
        m.manualAudio.resetLevels();
        m.probe.reset();
    }

    void advance(uint32_t ms) {
        // By the clock, not by passes: with card_speed a pass can take longer
        // than its 1 ms (the card command's time is in the clock already).
        const uint64_t end = m.clock.nowUs() + uint64_t(ms) * 1000u;
        while (m.clock.nowUs() < end) {
            m.clock.advanceUs(1000);
            if (m.clockBpm > 0.0) {   // midi_clock: 24 ticks a beat on DIN
                const double nowMs = double(m.clock.micros()) / 1000.0;
                while (nowMs >= m.nextTickMs) {
                    m.midi.in.push_back(0xF8);
                    m.nextTickMs += 60000.0 / (m.clockBpm * 24.0);
                }
            }
            frameAcc += 44.1;
            while (frameAcc >= 128.0) {
                m.manualAudio.pull();
                frameAcc -= 128.0;
            }
            m.app.loop();
            if (m.app.lastWords() > maxWords) maxWords = m.app.lastWords();
        }
    }
    uint32_t maxWords = 0;   // the busiest frame's display list, in words (FT81x: 2048)

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
            else if (cmd == "loopback") {
                // loopback FRAMES GAIN | loopback off: the output into the input, FRAMES later
                std::string g;
                in >> g;
                m.tone.loopDelay = arg == "off" ? 0 : uint32_t(std::atol(arg.c_str()));
                m.tone.loopGain = g.empty() ? 1.0f : float(std::atof(g.c_str()));
            } else if (cmd == "expect_latency") {
                // expect_latency FRAMES TOLERANCE: the measured round trip (Config > F3)
                std::string tol;
                in >> tol;
                const looper::Engine& e = m.app.engine();
                const int got = e.measureState() == 2 ? int(e.measured()) : -1;
                const bool pass = got >= 0 && std::abs(got - std::atoi(arg.c_str())) <= std::atoi(tol.c_str());
                std::printf("%s:%d: expect_latency %s %s (%d frames, state %d)\n", path.c_str(), lineNo, arg.c_str(),
                            pass ? "PASS" : "FAIL", got, int(e.measureState()));
                ok = ok && pass;
            } else if (cmd == "expect_baroffset") {
                // expect_baroffset TRACK FRAMES: where the first bar line falls in its loop
                std::string want;
                in >> want;
                const int t = std::atoi(arg.c_str()) - 1;
                const long got = t >= 0 && t < looper::kTracks ? long(m.app.engine().view(t).barOffset.load()) : -1;
                const bool pass = got == std::atol(want.c_str());
                std::printf("%s:%d: expect_baroffset %s %s %s (%ld)\n", path.c_str(), lineNo, arg.c_str(), want.c_str(),
                            pass ? "PASS" : "FAIL", got);
                ok = ok && pass;
            } else if (cmd == "expect_onsets") {
                // expect_onsets MIN MAX: clicks heard on the left output since the levels were reset
                std::string mx;
                in >> mx;
                const uint32_t n = m.probe.onsets();
                const bool pass = n >= uint32_t(std::atoi(arg.c_str())) && n <= uint32_t(std::atoi(mx.c_str()));
                std::printf("%s:%d: expect_onsets %s %s %s (%u)\n", path.c_str(), lineNo, arg.c_str(), mx.c_str(),
                            pass ? "PASS" : "FAIL", n);
                ok = ok && pass;
            } else if (cmd == "tone_ch") {
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
                static const char* const kNames[] = {"CLEAR", "EMPTY", "COUNT", "ARMED", "REC", "PLAY", "DUB", "STOP", "QUEUED"};
                std::string want;
                in >> want;
                const int t = std::atoi(arg.c_str()) - 1;
                const int s = t >= 0 && t < looper::kTracks ? int(m.app.engine().view(t).state.load()) : 0;
                const bool pass = s >= 0 && s < 9 && want == kNames[s];
                std::printf("%s:%d: expect_state %s %s %s (%s)\n", path.c_str(), lineNo, arg.c_str(), want.c_str(),
                            pass ? "PASS" : "FAIL", s >= 0 && s < 9 ? kNames[s] : "?");
                ok = ok && pass;
            } else if (cmd == "opt") {
                // opt TRACK|all NAME VALUE: a track option, as the options page sets it
                static const char* const kNames[] = {"stop",  "length",    "start", "input", "after",
                                                     "tempo", "beats", "metronome", "click", "threshold", "stretch", "stopfade", "monitorfx",
                                                     "accent", "clicksound", "clickwhen", "countin"};
                std::string name, val;
                in >> name >> val;
                int p = -1;
                for (int i = 0; i < 17; ++i)
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
            } else if (cmd == "sd_file") {
                // sd_file PATH BYTES: a file of zero bytes on the SD card (parent folders made)
                std::string bytes;
                in >> bytes;
                std::string dir;
                for (size_t i = 1; i < arg.size(); ++i)
                    if (arg[i] == '/') m.storage.mkdir(arg.substr(0, i).c_str());
                const ot::IStorage::Handle fh = m.storage.open(arg.c_str(), ot::IStorage::kWrite);
                if (fh >= 0) {
                    std::vector<uint8_t> z(std::strtoul(bytes.c_str(), nullptr, 10), 0);
                    m.storage.write(fh, z.data(), uint32_t(z.size()));
                    m.storage.close(fh);
                }
            } else if (cmd == "expect_pitch") {
                // expect_pitch HZ [TOL%]: the left output's frequency since reset_levels, within 3 % (or TOL)
                std::string tol;
                in >> tol;
                const float tl = tol.empty() ? 3.0f : float(std::atof(tol.c_str()));
                const float want = float(std::atof(arg.c_str())), got = m.probe.pitchHz();
                const bool pass = want > 0.0f && std::fabs(got - want) <= want * tl / 100.0f;
                std::printf("%s:%d: expect_pitch %s %s (%.1f Hz)\n", path.c_str(), lineNo, arg.c_str(),
                            pass ? "PASS" : "FAIL", got);
                ok = ok && pass;
            } else if (cmd == "expect_smooth") {
                // expect_smooth MAX: no sample-to-sample step on the left output above MAX since reset_levels
                const float want = float(std::atof(arg.c_str())), got = m.probe.maxStep();
                const bool pass = got <= want;
                std::printf("%s:%d: expect_smooth %s %s (max step %.4f)\n", path.c_str(), lineNo, arg.c_str(),
                            pass ? "PASS" : "FAIL", got);
                ok = ok && pass;
            } else if (cmd == "midi_in") {
                // midi_in HEX...: bytes in on DIN (e.g. midi_in 90 3C 7F)
                std::string hx = arg;
                do {
                    if (!hx.empty()) m.midi.in.push_back(uint8_t(std::strtoul(hx.c_str(), nullptr, 16)));
                } while (in >> hx);
            } else if (cmd == "midi_clock") {
                // midi_clock BPM [start]: a clock coming in on DIN from now (0: none); start sends Start first
                std::string st;
                in >> st;
                m.clockBpm = std::atof(arg.c_str());
                m.nextTickMs = double(m.clock.micros()) / 1000.0;
                if (st == "start") m.midi.in.push_back(0xFA);
                if (st == "stop") m.midi.in.push_back(0xFC);
            } else if (cmd == "mix") {
                // mix TRACK NAME VALUE: a mixer setting (level, send1, send2, return1, return2,
                // master, chain1, chain2; TRACK for level and sends)
                static const char* const kMixNames[] = {"level",  "send1",  "send2",  "return1",
                                                        "return2", "master", "chain1", "chain2"};
                std::string name, val;
                in >> name >> val;
                for (int i = 0; i < 8; ++i)
                    if (name == kMixNames[i])
                        m.app.postMix(looper::MixParam(i), std::atoi(arg.c_str()) - 1 < 0 ? 0 : std::atoi(arg.c_str()) - 1,
                                      std::atoi(val.c_str()));
            } else if (cmd == "reset_midi") {
                m.midi.out[0].clear();
                m.midi.out[1].clear();
            } else if (cmd == "expect_midi_out") {
                // expect_midi_out din|usb BYTE MIN [MAX]: how many BYTE (hex) went out since reset_midi
                std::string byteS, minS, maxS;
                in >> byteS >> minS >> maxS;
                const auto& o = m.midi.out[arg == "usb" ? 1 : 0];
                const uint8_t want = uint8_t(std::strtoul(byteS.c_str(), nullptr, 16));
                long cnt = 0;
                for (uint8_t b : o) cnt += b == want;
                const long lo = std::atol(minS.c_str()), hi = maxS.empty() ? 1000000 : std::atol(maxS.c_str());
                const bool pass = cnt >= lo && cnt <= hi;
                std::printf("%s:%d: expect_midi_out %s %s %s %s %s (%ld)\n", path.c_str(), lineNo, arg.c_str(),
                            byteS.c_str(), minS.c_str(), maxS.c_str(), pass ? "PASS" : "FAIL", cnt);
                ok = ok && pass;
            } else if (cmd == "expect_flash") {
                // expect_flash NAME|none: the image the last firmware update asked for
                const bool pass = arg == (gFlashed.empty() ? std::string("none") : gFlashed);
                std::printf("%s:%d: expect_flash %s %s (%s)\n", path.c_str(), lineNo, arg.c_str(), pass ? "PASS" : "FAIL",
                            gFlashed.empty() ? "none" : gFlashed.c_str());
                ok = ok && pass;
            } else if (cmd == "expect_file") {
                // expect_file PATH MINBYTES: a file on the SD card, at least that big
                std::string minb;
                in >> minb;
                const ot::IStorage::Handle fh = m.storage.open(arg.c_str(), ot::IStorage::kRead);
                const int32_t sz = fh >= 0 ? m.storage.size(fh) : -1;
                if (fh >= 0) m.storage.close(fh);
                const bool pass = sz >= std::atol(minb.c_str());
                std::printf("%s:%d: expect_file %s %s %s (%ld bytes)\n", path.c_str(), lineNo, arg.c_str(),
                            minb.c_str(), pass ? "PASS" : "FAIL", static_cast<long>(sz));
                ok = ok && pass;
            } else if (cmd == "expect_opt") {
                // expect_opt TRACK NAME VALUE: an option's value (names as for opt)
                static const char* const kNames[] = {"stop",  "length",    "start", "input", "after",
                                                     "tempo", "beats", "metronome", "click", "threshold", "stretch", "stopfade", "monitorfx",
                                                     "accent", "clicksound", "clickwhen", "countin"};
                std::string name, val;
                in >> name >> val;
                int p = -1;
                for (int i = 0; i < 17; ++i)
                    if (name == kNames[i]) p = i;
                const int t = std::atoi(arg.c_str()) - 1;
                int got = p >= 0 && t >= 0 && t < looper::kTracks ? m.app.engine().opt(looper::OptParam(p), t) : -1;
                // Settings that are not track options, under the same command.
                if (name == "monitor") got = m.app.engine().monitoring() ? 1 : 0;
                else if (name == "feedback") got = m.app.engine().feedback();
                else if (name == "gain") got = m.app.inputGain();
                else if (name == "volume" && t >= 0 && t < looper::kTracks) got = m.app.engine().volume(t);
                const bool pass = got == std::atoi(val.c_str());
                std::printf("%s:%d: expect_opt %s %s %s %s (%d)\n", path.c_str(), lineNo, arg.c_str(), name.c_str(),
                            val.c_str(), pass ? "PASS" : "FAIL", got);
                ok = ok && pass;
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
            } else if (cmd == "card_speed") {
                std::string r, lat, st, ev;
                in >> r >> lat >> st >> ev;
                wMBs = std::atof(arg.c_str());
                rMBs = std::atof(r.c_str());
                latMs = std::atof(lat.c_str());
                stallMs = std::atof(st.c_str());
                stallEvery = uint32_t(std::atol(ev.c_str()));
                if (wMBs > 0.0 && rMBs > 0.0) {
                    m.storage.ioHook = &Headless::ioHook;
                    m.storage.ioCtx = this;
                } else {
                    m.storage.ioHook = nullptr;
                }
            } else if (cmd == "reset_stream") {
                const int t = std::atoi(arg.c_str()) - 1;
                if (t >= 0 && t < looper::kTracks) {   // a test's own counters: the engine only ever adds to them
                    looper::StreamView& sv = const_cast<looper::StreamView&>(m.app.engine().stream(t));
                    sv.underruns.store(0);
                    sv.overruns.store(0);
                }
            } else if (cmd == "expect_stream") {
                const int t = std::atoi(arg.c_str()) - 1;
                std::string field, want;
                in >> field >> want;
                long got = -1;
                if (t >= 0 && t < looper::kTracks) {
                    const looper::StreamView& sv = m.app.engine().stream(t);
                    if (field == "long") got = m.app.engine().longTake(t) ? 1 : 0;
                    else if (field == "under") got = long(sv.underruns.load());
                    else if (field == "over") got = long(sv.overruns.load());
                }
                const long w = std::atol(want.c_str());
                const bool pass = got >= 0 && (field == "long" ? got == w : got <= w);
                std::printf("%s:%d: expect_stream %s %s %s %s (%ld)\n", path.c_str(), lineNo, arg.c_str(), field.c_str(),
                            want.c_str(), pass ? "PASS" : "FAIL", got);
                ok = ok && pass;
            } else if (cmd == "expect_mode") {
                // expect_mode TRACK NAME: the track's playback mode, its label without spaces
                const int t = std::atoi(arg.c_str()) - 1;
                std::string want, got;
                in >> want;
                if (t >= 0 && t < looper::kTracks)
                    for (const char* c = looper::modeInfo(m.app.engine().mode(t)).name; *c; ++c)
                        if (*c != ' ') got += *c;
                const bool pass = got == want;
                std::printf("%s:%d: expect_mode %s %s %s (%s)\n", path.c_str(), lineNo, arg.c_str(), want.c_str(),
                            pass ? "PASS" : "FAIL", got.c_str());
                ok = ok && pass;
            } else if (cmd == "expect_grain") {
                // expect_grain TRACK KNOB VALUE: Granular mode's knob (1 spread, 2 size, 3 density, 4 pitch)
                std::string kn, val;
                in >> kn >> val;
                const int t = std::atoi(arg.c_str()) - 1, k = std::atoi(kn.c_str()) - 1;
                const int got = t >= 0 && t < looper::kTracks && k >= 0 && k < looper::kGrainKnobs
                                    ? m.app.engine().grainParam(t, k) : -1;
                const bool pass = got == std::atoi(val.c_str());
                std::printf("%s:%d: expect_grain %s %s %s %s (%d)\n", path.c_str(), lineNo, arg.c_str(), kn.c_str(),
                            val.c_str(), pass ? "PASS" : "FAIL", got);
                ok = ok && pass;
            } else if (cmd == "expect_filter") {
                // expect_filter TRACK VALUE: the mixer's DJ filter, 0..1000 (500 the middle: off)
                std::string val;
                in >> val;
                const int t = std::atoi(arg.c_str()) - 1;
                const int got = t >= 0 && t < looper::kTracks ? int(m.app.engine().filterFine(t)) : -1;
                const bool pass = got == std::atoi(val.c_str());
                std::printf("%s:%d: expect_filter %s %s %s (%d)\n", path.c_str(), lineNo, arg.c_str(), val.c_str(),
                            pass ? "PASS" : "FAIL", got);
                ok = ok && pass;
            } else if (cmd == "expect_perfknob") {
                // expect_perfknob FX VALUE [TRACK]: performance effect FX (1..8) knob value (track 1..4, default 1)
                std::string val, tr;
                in >> val >> tr;
                const int e = std::atoi(arg.c_str()) - 1, t = tr.empty() ? 0 : std::atoi(tr.c_str()) - 1;
                const int got = e >= 0 && e < looper::kPerfFxCount && t >= 0 && t < looper::kTracks
                                    ? m.app.engine().perfParam(t, looper::PerfFx(e)) : -1;
                const bool pass = got == std::atoi(val.c_str());
                std::printf("%s:%d: expect_perfknob %s %s %s (%d)\n", path.c_str(), lineNo, arg.c_str(), val.c_str(),
                            pass ? "PASS" : "FAIL", got);
                ok = ok && pass;
            } else if (cmd == "tone_shape") m.tone.steady = arg == "steady";
            else if (cmd == "input_wav") {
                if (arg == "off") m.tone.wav.clear();
                else if (!m.tone.loadWav(arg)) {
                    std::fprintf(stderr, "%s:%d: cannot read %s\n", path.c_str(), lineNo, arg.c_str());
                    ok = false;
                }
            }
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
        std::printf("%s: display list max %u words of 2048%s\n", path.c_str(), unsigned(maxWords),
                    maxWords > 1900 ? " FAIL (too close to the limit)" : "");
        ok = ok && maxWords <= 1900;
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

// ---------------------------------------------------------------------------
// Soak: a long random session. A seeded "user" presses keys and pads, turns
// the encoder, changes the input, records long takes on a slow card, saves
// and loads; after every action the engine's state is checked, and every
// ten minutes a project is saved, loaded back and compared. The same seed
// replays the same session (virtual time), so a failure can be run again.

bool soak(Headless& h, uint32_t seed, uint32_t minutes, const std::string& logPath) {
    Machine& m = h.m;
    std::mt19937 rng(seed);
    auto rnd = [&](uint32_t lo, uint32_t hi) { return lo + uint32_t(rng() % (hi - lo + 1)); };   // [lo, hi]
    auto chance = [&](uint32_t perMille) { return rnd(0, 999) < perMille; };
    FILE* log = logPath.empty() ? nullptr : std::fopen(logPath.c_str(), "w");
    std::vector<std::string> recent;   // the last actions, for a failure report
    auto act = [&](const std::string& what) {
        char t[32];
        std::snprintf(t, sizeof t, "%9.3f ", double(m.clock.nowUs()) / 1e6);
        recent.push_back(t + what);
        if (recent.size() > 40) recent.erase(recent.begin());
        if (log) std::fprintf(log, "%s%s\n", t, what.c_str());
    };
    auto btn = [&](const char* n) { return uint8_t(buttonByName(n)); };
    auto press = [&](const char* n) {
        act(std::string("press ") + n);
        m.input.button(btn(n), true);
        h.advance(30);
        m.input.button(btn(n), false);
    };
    auto tap = [&](int pad) {
        act("tap " + std::to_string(pad));
        m.input.pad(uint8_t(pad), true, 100);
        h.advance(20);
        m.input.pad(uint8_t(pad), false);
    };
    auto enc = [&](int d) {
        act("enc " + std::to_string(d));
        m.input.encoder(d);
        h.advance(5);
    };
    // A slow card, like the 8 GB test card (h.ioHook is installed by card_speed).
    h.wMBs = 8;
    h.rMBs = 16;
    h.latMs = 1;
    h.stallMs = 150;
    h.stallEvery = 64;
    m.storage.ioHook = &Headless::ioHook;
    m.storage.ioCtx = &h;
    m.tone.steady = true;
    const char* wavs[] = {"../build/demo/drums.wav", "../build/demo/bass.wav", "../build/demo/chords.wav",
                          "../build/demo/pad.wav"};

    std::string why;
    auto fail = [&](const std::string& w) {
        if (!why.empty()) return;
        why = w;
        if (!logPath.empty()) panelShot(m, logPath + ".png", false);   // the screen as it failed
    };
    auto check = [&]() {
        const looper::Engine& e = m.app.engine();
        for (int t = 0; t < looper::kTracks; ++t) {
            const looper::TrackView& v = e.view(t);
            const int s = int(v.state.load());
            const uint32_t len = v.len.load(), start = v.start.load();
            const bool mono = v.mono.load(), lng = e.longTake(t);
            const uint32_t ram = (mono ? 2 : 1) * e.memory(t).capacity;
            const uint32_t most = lng ? (mono ? 2 : 1) * looper::kStreamFrames : ram;
            char b[160];
            if (s < 0 || s > 8) {
                std::snprintf(b, sizeof b, "track %d: state %d", t + 1, s);
                fail(b);
            }
            const bool loop = s == 5 || s == 6 || s == 7 || s == 8;   // PLAY DUB STOP QUEUED
            if (loop && !len) {
                std::snprintf(b, sizeof b, "track %d: state %d with no loop", t + 1, s);
                fail(b);
            }
            if (len && uint64_t(start) + len > most) {
                std::snprintf(b, sizeof b, "track %d: loop [%u, %u) past its %s (%u)", t + 1, start, start + len,
                              lng ? "stream file" : "RAM", most);
                fail(b);
            }
        }
        if (m.app.streamer().errors()) fail("card command errors: " + std::to_string(m.app.streamer().errors()));
        if (h.maxWords > 2048) fail("display list over 2048 words: " + std::to_string(h.maxWords));
    };
    auto waitIdle = [&](uint32_t maxMs) {   // a File job done
        for (uint32_t w = 0; w < maxMs && m.app.files().busy(); w += 100) h.advance(100);
    };
    // Save a new project, load it back, compare the loops' lengths and channels.
    auto roundTrip = [&]() {
        act("-- save and load back");
        press("PATTERN");
        waitIdle(120000);
        press("FILE");
        press("F2");
        h.advance(200);
        waitIdle(300000);
        const int saved = m.app.files().current();
        if (!saved) {
            act(std::string("   (not saved: ") + m.app.files().status() + ")");
            press("PATTERN");
            return;
        }
        uint32_t len[looper::kTracks];
        bool mono[looper::kTracks];
        for (int t = 0; t < looper::kTracks; ++t) {
            len[t] = m.app.engine().view(t).len.load();
            mono[t] = m.app.engine().view(t).mono.load();
        }
        press("F3");
        press("F3");
        h.advance(200);
        waitIdle(300000);
        h.advance(1500);
        if (m.app.files().current() == saved) {
            for (int t = 0; t < looper::kTracks; ++t) {
                const uint32_t l = m.app.engine().view(t).len.load();
                if (l != len[t] || (l && m.app.engine().view(t).mono.load() != mono[t])) {
                    char b[120];
                    std::snprintf(b, sizeof b, "project %d: track %d came back %u frames%s, saved %u", saved, t + 1, l,
                                  m.app.engine().view(t).mono.load() ? " mono" : "", len[t]);
                    fail(b);
                }
            }
        } else {
            act(std::string("   (loaded something else: ") + m.app.files().status() + ")");
        }
        press("PATTERN");
    };

    // Silence a listener hears: underruns on a track playing (or overdubbing),
    // unmuted, at both ends of an action; by mode, to see where they come from.
    uint64_t audible = 0, audibleBy[16] = {};
    uint32_t underAt[looper::kTracks] = {};
    auto playing = [&](int t) {
        const looper::TrackView& v = m.app.engine().view(t);
        const int s = int(v.state.load());
        return (s == 5 || s == 6) && !v.muted.load() && m.app.engine().longTake(t);
    };
    bool wasPlaying[looper::kTracks] = {};
    auto noteUnder = [&](bool before) {
        for (int t = 0; t < looper::kTracks; ++t) {
            const uint32_t u = m.app.engine().stream(t).underruns.load();
            if (!before && wasPlaying[t] && playing(t) && u > underAt[t]) {
                audible += u - underAt[t];
                audibleBy[m.app.engine().view(t).mode.load() & 15] += u - underAt[t];
                if (u - underAt[t] > 4410) {   // over 0.1 s: where, for the log
                    const looper::TrackView& v = m.app.engine().view(t);
                    const looper::StreamView& sv = m.app.engine().stream(t);
                    char b[200];
                    std::snprintf(b, sizeof b,
                                  "   heard %.2f s of silence on track %d: mode %d state %d pos %u start %u len %u "
                                  "head %u ring [%u, %u) back %d",
                                  double(u - underAt[t]) / 44100.0, t + 1, int(v.mode.load()), int(v.state.load()),
                                  v.pos.load(), v.start.load(), v.len.load(), sv.head.load(), sv.rStart.load(),
                                  sv.rEnd.load(), int(sv.back.load()));
                    act(b);
                }
            }
            underAt[t] = u;
            wasPlaying[t] = playing(t);
        }
    };
    const uint64_t endUs = m.clock.nowUs() + uint64_t(minutes) * 60000000u;
    uint64_t nextTrip = m.clock.nowUs() + 600000000u;   // every 10 minutes
    uint32_t actions = 0;
    bool shift = false;
    while (m.clock.nowUs() < endUs && why.empty()) {
        ++actions;
        noteUnder(true);
        const uint32_t r = rnd(0, 999);
        if (r < 170) {   // a pad: record, play, mute, clear, the jumps / performance effects
            const int row = int(rnd(0, 3));
            const uint32_t c = rnd(0, 99);
            const int col = c < 35 ? 0 : c < 55 ? 1 : c < 65 ? 2 : c < 67 ? 3 : int(rnd(4, 11));
            tap(row * 12 + col);
        } else if (r < 220) {   // a pad held (a performance effect, its knob)
            const int pad = int(rnd(0, 3)) * 12 + int(rnd(4, 11));
            act("hold pad " + std::to_string(pad));
            m.input.pad(uint8_t(pad), true, 100);
            h.advance(rnd(100, 1500));
            if (chance(500)) enc(int(rnd(0, 8)) - 4);
            h.advance(rnd(50, 800));
            m.input.pad(uint8_t(pad), false);
        } else if (r < 330) {   // a screen key
            static const char* const kF[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8"};
            press(kF[rnd(0, 7)]);
        } else if (r < 370) {   // a screen key held, the encoder turned (tempo, grain menu, gain, volume)
            static const char* const kH[] = {"F4", "F5", "F6", "F7"};
            const char* k = kH[rnd(0, 3)];
            act(std::string("hold ") + k);
            m.input.button(btn(k), true);
            h.advance(rnd(400, 900));
            for (uint32_t i = 0, n = rnd(1, 4); i < n; ++i) enc(int(rnd(0, 12)) - 6);
            m.input.button(btn(k), false);
            act(std::string("release ") + k);
        } else if (r < 450) {
            enc(int(rnd(0, 20)) - 10);
        } else if (r < 480) {
            shift = !shift;
            act(shift ? "hold SHIFT" : "release SHIFT");
            m.input.button(btn("SHIFT"), shift);
        } else if (r < 520) {
            press("REC");
        } else if (r < 550) {
            press("PLAY");
        } else if (r < 570) {
            press("INSERT");
        } else if (r < 576) {
            press("DELETE");
        } else if (r < 600) {
            press(chance(500) ? "UP" : "DOWN");
        } else if (r < 615) {
            press(chance(500) ? "LEFT" : "RIGHT");
        } else if (r < 735) {   // pages
            // (Pattern is home, Note and S.Edit the editor, Instr a second Perform;
            // Copy duplicates a track or copies in the editor, S.Load the sample loader.)
            static const char* const kP[] = {"PATTERN", "PATTERN", "PATTERN", "MASTER", "NOTE", "SAMPLEEDITOR",
                                             "FX1", "FX2", "FILE", "SONG", "PERFORMANCE", "INSTR", "HELP",
                                             "COPY", "COPY", "SAMPLELOADER"};
            press(kP[rnd(0, 15)]);
        } else if (r < 745) {   // tap tempo
            for (int i = 0; i < 4; ++i) {
                press("ENCPUSH");
                h.advance(rnd(380, 700));
            }
        } else if (r < 800) {   // the input
            const uint32_t k = rnd(0, 9);
            if (k < 3) {
                const float hz = float(rnd(110, 990));
                m.tone.wav.clear();
                m.tone.hz = hz;
                act("tone " + std::to_string(int(hz)));
            } else if (k < 6) {
                const char* w = wavs[rnd(0, 3)];
                if (m.tone.loadWav(w)) act(std::string("input_wav ") + w);
            } else {
                m.tone.wav.clear();
                m.tone.hz = 0;
                act("tone off");
            }
        } else if (r < 812) {   // a long take on one track: past its RAM, onto the card
            const int row = int(rnd(0, 3));
            tap(row * 12);
            h.advance(rnd(21000, 45000));
            tap(row * 12);
        } else if (r < 830) {
            const uint32_t ms = rnd(3000, 20000);
            act("wait " + std::to_string(ms));
            h.advance(ms);
        } else {
            // nothing: just time passing below
        }
        h.advance(rnd(30, 1200));
        noteUnder(false);
        check();
        if (why.empty() && m.clock.nowUs() >= nextTrip) {
            nextTrip += 600000000u;
            if (shift) {
                m.input.button(btn("SHIFT"), false);
                shift = false;
            }
            roundTrip();
            check();
        }
    }
    if (shift) m.input.button(btn("SHIFT"), false);
    const looper::Engine& e = m.app.engine();
    uint32_t under = 0, over = 0, dropped = 0;
    for (int t = 0; t < looper::kTracks; ++t) {
        under += e.stream(t).underruns.load();
        over += e.stream(t).overruns.load();
        dropped += e.stream(t).dropped.load();
    }
    std::printf("soak seed %u: %.1f min, %u actions, display list max %u words, stream under %u (heard %.1f s) "
                "over %u dropped %u: %s\n",
                seed, double(m.clock.nowUs()) / 60e6, actions, h.maxWords, under, double(audible) / 44100.0, over,
                dropped, why.empty() ? "PASS" : "FAIL");
    for (int md = 0; md < 16; ++md)
        if (audibleBy[md]) std::printf("  heard silence in mode %d: %.1f s\n", md, double(audibleBy[md]) / 44100.0);
    if (!why.empty()) {
        std::printf("  %s\n  the last actions:\n", why.c_str());
        for (const std::string& a : recent) std::printf("    %s\n", a.c_str());
    }
    if (log) std::fclose(log);
    return why.empty();
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parseArgs(argc, argv, o)) return 2;
    if (o.sd.empty()) {   // the card: a folder "sd" next to the program
        std::string self = argv[0];
        const size_t slash = self.find_last_of('/');
        o.sd = (slash == std::string::npos ? std::string(".") : self.substr(0, slash)) + "/sd";
    }
    const bool headless = !o.script.empty() || o.soakSeed >= 0;
    static Machine m;   // 16 MB of loop memory: keep it off the stack
    if (!m.start(o, headless)) return 1;
    if (!headless) return runInteractive(m, o);
    Headless h{m};
    h.advance(300);   // boot: zero the loop memory, first frames
    // The scripts are timed at 120 bpm (a 2 s bar), not the device's default tempo.
    m.app.scriptOpt(0, looper::OptParam::kTempo, 120);
    h.advance(10);
    const bool ok = o.soakSeed >= 0 ? soak(h, uint32_t(o.soakSeed), uint32_t(o.soakMinutes), o.soakLog) : h.run(o.script);
    h.advance(50);
    if (!o.shot.empty()) panelShot(m, o.shot, false);
    if (m.audio) m.audio->stop();
    return ok ? 0 : 1;
}
