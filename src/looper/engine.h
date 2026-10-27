// Looper engine: four stereo loop tracks locked to one master length.
//
// Threading: render() runs in the audio callback (I2S ISR on the device, the
// SDL audio thread in the simulator). The main loop talks to it only through
// post() (a lock-free single-producer queue drained at the start of each
// block) and reads the published TrackView / meter fields. Zeroing a cleared
// track's buffer is slow (megabytes of SDRAM), so service() does it from the
// main loop in chunks while the track sits in kClearing.
//
// Timing: one global frame counter t runs while the transport plays. The
// first recording sets the master length L and t = 0. Every other track is
// anchored to a multiple of L and its length is rounded up to a multiple of L.
// A track's phase is a function of t only:
//   phase = ((t - anchor) * timeRate + offset) mod period      (16.16 fixed point)
// so every playback mode stays locked to the bar clock. The mode maps the
// phase to a read position (reverse, ping-pong) and picks the reader: direct,
// interpolated (tape speed) or two cross-faded grains (stretch / pitch shift).
#pragma once

#include <atomic>
#include <cstdint>

#include "looper/fx.h"
#include "looper/clouds/grain.h"

namespace looper {

constexpr int kTracks = 4;
constexpr int kFxSlots = 2;                  // FX 1, FX 2 (any FxType each), fed by per-track sends
constexpr int kSegments = 8;                 // pad columns 4..11 jump between eighths
constexpr uint32_t kPeakFrames = 256;        // frames per waveform peak cell
constexpr uint32_t kSampleRate = 44100;
constexpr uint32_t kGrainFrames = 2048;      // granular modes: 46 ms grains, 50 % overlap

enum class TrackState : uint8_t {
    kClearing, kEmpty, kCountIn, kArmed, kRecording, kPlaying, kOverdubbing, kStopped,
    kQueued,   // stopped, starts from its beginning on the next bar line
};

// Per-track recording options (Engine::kOpt). They stay when the track is cleared.
enum class StopQuant : uint8_t { kFree, kBar, kBeat };   // where a stop press ends the take
enum class StartMode : uint8_t { kNow, kCountIn, kSound };   // how a press starts the take
enum class InputCh : uint8_t { kStereo, kMonoL, kMonoR, kMonoSum };
enum class AfterRec : uint8_t { kPlay, kOverdub, kStop };
struct TrackOpts {
    StopQuant stop = StopQuant::kBar;
    uint8_t lenBeats = 0;        // fixed length in beats, 0 = off (stop by hand)
    StartMode start = StartMode::kCountIn;   // count-in: one bar of click, then record from the bar line
    InputCh input = InputCh::kStereo;
    AfterRec after = AfterRec::kPlay;
    bool stretch = false;        // at a changed tempo: keep the pitch (grains) instead of tape
};
enum class OptParam : uint8_t {
    kStop, kLength, kStart, kInput, kAfter,               // per track
    kTempo, kBeats, kMetronome, kClick, kThreshold,       // global: BPM 40..240, beats per bar 1..12,
                                                          // click on/off, level, sound threshold 0..100
    kStretch,                                             // per track: 0 tape, 1 stretch (keeps pitch)
    kStopFade,                                            // global: stop fades over 0 off, 1 beat, 1 bar, 2 bars
    kMonitorFx,                                           // global: monitored input to bit 0 FX 1, bit 1 FX 2
};
constexpr uint8_t kMaxLenBeats = 64;
// Sound start: threshold 0..100 maps to -60..0 dBFS; the take keeps this much
// input from just before the trigger, so the attack is not cut.
inline float thresholdDb(uint8_t v) { return -60.0f + 0.6f * float(v > 100 ? 100 : v); }
constexpr uint32_t kPreRollFrames = 256;   // 5.8 ms
// Loop point: a take records this much past its end, and that tail is blended
// into its first frames, so the end runs into what really came next.
constexpr uint32_t kXfadeFrames = 220;     // 5 ms
// A take that starts after silence in its loop (mid-bar) fades in over this.
constexpr uint32_t kFadeInFrames = 88;     // 2 ms
// Overdub and replace ramp in and out over this (no click at the punch points).
constexpr float kDubRampStep = 1.0f / 88.0f;   // 2 ms
// Overdub undo: the loop's audio under an overdub is saved a cell at a time,
// the first time the overdub touches the cell, at the end of the track's own
// buffer. So undo needs the loop to fit twice: up to half the track's memory.
constexpr uint32_t kUndoCellFrames = 256;
constexpr uint32_t kUndoMapBytes = 512;    // cells of a loop of up to 2^20 frames (half of the largest buffer)

// Performance effects (Engine::kPerf), per track, held from the pad grid.
// Repeats, tape stop and vibrato move the read position; the phase underneath
// keeps running, so releasing one lands back in time.
enum class PerfFx : uint8_t {
    kRepeat8, kRepeat16, kLowPass, kHighPass, kSweepDown, kSweepUp, kVibrato, kTapeStop, kCount
};
constexpr int kPerfFxCount = int(PerfFx::kCount);
constexpr const char* kPerfFxName[kPerfFxCount] = {"Repeat",   "Repeat",   "Low-pass", "High-pass",
                                                   "Sweep dn", "Sweep up", "Vibrato",  "Tape stop"};

// One knob per performance effect (Engine::kPerfParam), shared by the tracks;
// set by holding the effect's pad and turning the encoder.
struct PerfKnob {
    const char* name;
    uint8_t max, def;
    const char* const* list;   // value names, or nullptr for 0..100
};
constexpr const char* kRepeatNames[] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
constexpr uint8_t kRepeatDiv[] = {1, 2, 3, 4, 6, 8};   // slice = beat / div
constexpr const char* kSweepNames[] = {"1/4 bar", "1/2 bar", "1 bar", "2 bars", "4 bars"};
constexpr float kSweepBars[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
constexpr const char* kTapeNames[] = {"1 beat", "2 beats", "1 bar", "2 bars"};
constexpr PerfKnob kPerfKnobs[kPerfFxCount] = {
    {"Length", 5, 1, kRepeatNames}, {"Length", 5, 3, kRepeatNames},   // 1/8 and 1/16
    {"Cutoff", 100, 38, nullptr},   {"Cutoff", 100, 66, nullptr},     // ~450 Hz, ~1.8 kHz
    {"Time", 4, 2, kSweepNames},    {"Time", 4, 2, kSweepNames},      // one bar
    {"Depth", 100, 50, nullptr},    {"Time", 3, 1, kTapeNames},       // two beats
};
inline float lowPassHz(uint8_t v) { return 80.0f * __builtin_powf(100.0f, float(v) / 100.0f); }    // 80 Hz..8 kHz
inline float highPassHz(uint8_t v) { return 100.0f * __builtin_powf(80.0f, float(v) / 100.0f); }   // 100 Hz..8 kHz

enum class PlayMode : uint8_t {
    kForward, kReverse, kPingPong, kHalfSpeed, kDoubleSpeed, kStretch, kOctaveUp, kOctaveDown, kGranular,
    kHalfReverse, kDoubleReverse, kCount   // new modes go last: projects store the number
};
constexpr int kModeCount = int(PlayMode::kCount);

struct ModeInfo {
    enum Reader : uint8_t { kDirect, kTape, kGrains, kCloud };   // kCloud: Clouds grains around the playhead
    const char* name;       // F-key label
    Reader reader;
    uint32_t time16;        // phase advance per output frame, 16.16 (tempo)
    uint32_t pitch16;       // grain read rate, 16.16 (kGrains only)
    bool pingPong;          // period = 2 * len
    bool reverse;
    bool canDub;            // overdub writes where it reads: only at speed 1
};

constexpr ModeInfo kModes[kModeCount] = {
    {"Forward", ModeInfo::kDirect, 65536, 65536, false, false, true},
    {"Reverse", ModeInfo::kDirect, 65536, 65536, false, true, true},
    {"Ping-pong", ModeInfo::kDirect, 65536, 65536, true, false, true},
    {"Half speed", ModeInfo::kTape, 32768, 32768, false, false, false},
    {"Double", ModeInfo::kTape, 131072, 131072, false, false, false},
    {"Half time", ModeInfo::kGrains, 32768, 65536, false, false, false},   // half tempo, same pitch
    {"Octave up", ModeInfo::kGrains, 65536, 131072, false, false, false},
    {"Octave dn", ModeInfo::kGrains, 65536, 32768, false, false, false},
    {"Granular", ModeInfo::kCloud, 65536, 65536, false, false, false},     // knobs: kGrainKnobInfo
    {"Half rev", ModeInfo::kTape, 32768, 32768, false, true, false},       // tape style, backwards
    {"Double rev", ModeInfo::kTape, 131072, 131072, false, true, false},
};

// Granular mode's knobs (per track, the grain submenu: hold F5): grains scattered around the
// playhead, so the loop keeps its time.
constexpr int kGrainKnobs = 4;
constexpr int kLoopGrains = 8;          // grains per track at most (each reads the SDRAM)
constexpr int kGrainPitchZero = 24;     // pitch 0..48 = -24..+24 semitones
struct GrainKnob {
    const char* name;
    uint8_t max, def;
};
constexpr GrainKnob kGrainKnobInfo[kGrainKnobs] = {
    {"Spread", 100, 0},                 // 0: at the playhead .. 100: anywhere in the loop
    {"Size", 100, 40},                  // 20 .. 500 ms
    {"Density", 100, 50},               // 1 .. 8 grains overlapping
    {"Pitch", 48, kGrainPitchZero},
};
inline const ModeInfo& modeInfo(PlayMode m) { return kModes[int(m) < kModeCount ? int(m) : 0]; }

// Mixer parameters (Engine::kMix). Track parameters use the command's track field.
enum class MixParam : uint8_t {
    kLevel,          // track level 0..100 (same as kVolume)
    kSend1,          // track -> FX 1, pre-fader (but muted with the track), 0..100
    kSend2,          // track -> FX 2
    kReturn1,        // FX 1 return level 0..100
    kReturn2,
    kMaster,         // master level 0..100
    kChain1,         // FX 1's output into FX 2 (before FX 1's return), 0..100
    kChain2,         // FX 2's output into FX 1; one way at a time (kChain1 wins)
};

// Effect memory the app hands to Engine::init (SDRAM on the device): a delay
// line per slot, read in order. Reverb lines, read at eight places per sample,
// are internal SRAM (engine.cpp): the SDRAM is uncached behind a 16-bit bus.
constexpr uint32_t kFxMemBytes = kFxSlots * Delay::kBytes;

// Long takes (docs/SD_STREAMING.md, phase 1). A take that outgrows its RAM
// keeps its first `head` frames there; the rest goes to the track's stream file
// on the card through a ring (the last kStreamRingFrames of its RAM). The
// main loop's Streamer drains the ring to the file while recording and keeps
// it filled around the playhead while playing; the audio thread only ever
// touches RAM.
constexpr uint32_t kStreamSeconds = 300;                           // a stream file: 5 min stereo, 10 mono
constexpr uint32_t kStreamFrames = kStreamSeconds * kSampleRate;   // stereo frames (4 bytes each)
constexpr uint32_t kStreamBlockBytes = 32768;                      // a card command, a block of the files
constexpr uint32_t kStreamBlocks = (kStreamFrames * 4u + kStreamBlockBytes - 1) / kStreamBlockBytes;
constexpr uint32_t kStreamRingFrames = 11 * (kStreamBlockBytes / 4);   // ~2 s: 11 blocks
constexpr uint32_t kMaxLatencyFrames = kSampleRate * 30 / 1000;   // latency compensation up to 30 ms
constexpr uint32_t kStreamBehind = kSampleRate / 2;                // the ring keeps 0.5 s behind the playhead

struct TrackMemory {
    int16_t* frames = nullptr;   // interleaved stereo
    uint32_t capacity = 0;       // frames
    uint8_t* peaks = nullptr;    // 2 bytes (left, right) per kPeakFrames frames, 0..255, for
    uint32_t peakBytes = 0;      //   the RAM and a long take's stream file both
};

// A track's take on the card, shared by the audio thread and the main loop's
// Streamer. Units are the track's own: frames, or samples on a mono track.
// With a stream file ready, every take is written to it as it records (file
// A); the one that outgrows its RAM becomes a long take. Its overdubs mark
// the 32 KB blocks they change; the Streamer writes each block's first change
// in a pass to the other file (A / B), so the pass before stays for undo.
struct StreamView {
    static constexpr uint32_t kBlockWords = (kStreamBlocks + 31u) / 32u;
    std::atomic<uint32_t> fileFrames{0};    // main: the stream file is ready, stereo frames (0: none)
    std::atomic<bool> undoFiles{false};     // main: the second file is ready too (undo on the card)
    std::atomic<bool> active{false};        // audio: this take is (being) mirrored on the card
    std::atomic<uint32_t> take{0};          // audio: counts takes (the Streamer starts each one afresh)
    std::atomic<uint32_t> head{0};          // audio: > 0 = a long take, loop frames [0, head) in RAM
    std::atomic<uint32_t> ring{0};          // audio: ring size (loop frame i >= head sits at head + (i - head) % ring)
    std::atomic<bool> recording{false};     // audio: the take (and its loop-point tail) is still going
    std::atomic<uint32_t> recorded{0};      // audio: frames recorded (the newest are in the ring)
    std::atomic<uint32_t> len{0};           // audio: the loop length, once recording is over
    std::atomic<uint32_t> flushed{0};       // main: loop frames of the take on the card (from 0)
    std::atomic<uint32_t> rStart{0}, rEnd{0};   // main: loop frames the ring holds for playback
    std::atomic<bool> back{false};          // audio: the playhead reads backwards (reverse, ping-pong's way back)
    // audio: a beat repeat or a tape stop reads here (buffer frame), away from
    // the playhead: the ring follows it while it lasts (kNoFx: none).
    static constexpr uint32_t kNoFx = 0xFFFFFFFFu;
    std::atomic<uint32_t> fxAt{kNoFx};
    std::atomic<uint32_t> underruns{0};     // audio: frames played as silence (not in the ring)
    std::atomic<uint32_t> overruns{0};      // audio: takes cut short (the card fell a ring behind)
    // audio: blocks an overdub changed (main writes them back), one map per
    // pass by session parity, so a pass's blocks stay its own for undo even
    // when the next pass starts before they are back on the card.
    std::atomic<uint32_t> dirty[2][kBlockWords];
    std::atomic<uint32_t> dropped{0};       // audio: overdub frames the ring did not hold (lost)
    std::atomic<uint32_t> session{0};       // audio: a new overdub pass began (a new undo set)
    std::atomic<bool> loopPoint{false};     // audio: the loop-point blend changed the take's first frames
    std::atomic<uint8_t> undoReq{0};        // audio: 1 undo, 2 redo the last pass (the Streamer swaps it)
    std::atomic<bool> undoDone{false};      // main: the swap is in (the track plays again)
    // A cut or erase of loop frames [editA, editB) on a long take, done on the
    // card (the track silent meanwhile): 1 cut, 2 erase.
    std::atomic<uint8_t> editReq{0};
    std::atomic<bool> editDone{false};      // main: the edit is in
    uint32_t editA = 0, editB = 0;          // audio, before editReq
};

// Snapshot the UI reads; written by the audio thread only.
struct TrackView {
    std::atomic<uint8_t> state{uint8_t(TrackState::kClearing)};
    std::atomic<bool> muted{false};
    std::atomic<uint8_t> mode{0};        // PlayMode
    std::atomic<uint32_t> len{0};        // loop length, frames (0 while empty / recording)
    std::atomic<uint32_t> written{0};    // frames recorded so far (recording) or len
    std::atomic<uint32_t> pos{0};        // play index of the last block
    std::atomic<uint32_t> peakVersion{0};
    std::atomic<uint8_t> level{0};       // output peak of the last block, 0..255
    std::atomic<bool> mono{false};       // buffer holds one channel (one peak byte per cell)
    std::atomic<uint32_t> startIn{0};    // count-in: frames until recording starts
    std::atomic<uint32_t> stopIn{0};     // recording: frames until the pending stop (0 = none)
    std::atomic<uint32_t> barOffset{0};  // buffer index of the first bar line (takes that start off a bar)
    std::atomic<uint8_t> perf{0};        // active PerfFx bits
    std::atomic<uint32_t> start{0};      // buffer index of the loop's first frame (trim)
    std::atomic<bool> busy{false};       // an edit is moving the audio: silent meanwhile
    std::atomic<bool> canUndo{false};    // a trim can be undone
    std::atomic<bool> replace{false};    // overdubbing replaces instead of adding
    std::atomic<uint8_t> dubUndo{0};     // 0 nothing, 1 the last overdub can be undone, 2 redone
};

class Engine {
public:
    enum Cmd : uint8_t {
        kRecord,       // empty -> record, record -> play, play <-> overdub, stopped -> overdub;
                       // arg 1: replace instead of overdub (punch-in)
        kPlayStop,     // play/overdub -> stopped, stopped -> play, record -> play
        kMute,
        kClear,
        kJump,         // arg = segment 0..7, keeps the phase inside the segment
        kVolume,       // arg = 0..100
        kTransport,    // start/stop all; restart from bar 0
        kClearAll,
        kMonitor,      // arg = 0/1 input monitoring
        kFeedback,     // arg = 0..100 overdub feedback
        kMode,         // arg = PlayMode; the read position carries on from where it was
        kMix,          // arg = (MixParam << 8) | value
        kOpt,          // arg = (OptParam << 8) | value
        kPerf,         // arg = (PerfFx << 8) | on (0/1)
        kPerfOff,      // every performance effect off on every track
        kPerfParam,    // arg = (PerfFx << 8) | value
        kFxType,       // track = slot, arg = FxType
        kFxKnob,       // track = slot, arg = (knob << 8) | value
        // Loop edits; a, b = selection in loop frames [a, b).
        kEditTrim,     // keep [a, b): instant, nothing is moved (the loop starts later in its buffer)
        kEditUndo,     // undo the last trim
        kEditCut,      // remove [a, b): the rest moves up (main loop job), silent meanwhile
        kEditErase,    // silence [a, b) (main loop job)
        // Project load: the app has written an empty track's audio and peaks.
        kInstall,      // a = length, b = bar phase of its start, arg = mono | stopped << 1 | long << 2
                       // (long: past its RAM, the whole take is in its stream file A already)
        kSetBar,       // a = bar length (the master loop), only while every track is empty
        kSetMute,      // arg = 0 / 1
        kGrainParam,   // arg = (knob << 8) | value, Granular mode's knobs
        kMultiply,     // arg 1: double the loop (a copy of it after it, main loop job), 0: halve it
        kDubUndo,      // arg 0: undo the last overdub / replace, 1: redo it (main loop job, silent)
        kExtTempo,     // a = tempo in bpm, 16.16 (MIDI clock in): the bar before a loop, the tape speed after
        kClockOut,     // arg 0/1: send MIDI clock (24 a beat), start and stop
        kLatency,      // arg = round-trip latency in output frames (0..kMaxLatencyFrames)
        kMeasure,      // arg 1: measure the round-trip latency (a cable from out to in), 0: stop
    };

    // fxMem: kFxMemBytes, 4-byte aligned.
    void init(const TrackMemory mem[kTracks], uint8_t* fxMem);
    void post(Cmd c, uint8_t track = 0, int16_t arg = 0, uint32_t a = 0, uint32_t b = 0);   // main loop only
    void service();   // main loop: zero cleared buffers, run edit jobs
    bool editBusy() const { return job_.state.load(std::memory_order_acquire) != 0; }
    // Latency measurement (kMeasure): 0 idle, 1 running, 2 done (measured(),
    // output frames), 3 no click came back.
    uint8_t measureState() const { return measState_.load(std::memory_order_acquire); }
    uint16_t measured() const { return measFrames_.load(std::memory_order_relaxed); }
    void render(int16_t* out, const int16_t* in, uint32_t frames);   // audio thread

    const TrackView& view(int t) const { return view_[t]; }
    const TrackMemory& memory(int t) const { return mem_[t]; }
    StreamView& stream(int t) { return stream_[t]; }   // the main loop's Streamer
    const StreamView& stream(int t) const { return stream_[t]; }
    bool longTake(int t) const { return stream_[t].head.load(std::memory_order_relaxed) != 0; }
    // Where a take on track t would go on to the card (its RAM part), mono or not.
    uint32_t headFor(int t, bool mono) const {
        const uint32_t cap = mono ? 2 * mem_[t].capacity : mem_[t].capacity, ring = mono ? 2 * kStreamRingFrames : kStreamRingFrames;
        const uint32_t bu = mono ? kStreamBlockBytes / 2 : kStreamBlockBytes / 4;
        return cap > 2 * ring ? (cap - ring) / bu * bu : 0;
    }
    // The modes a long take can play: all of them (Granular's grains stay
    // where the RAM part or the ring holds them).
    static bool streamMode(PlayMode) { return true; }
    uint32_t masterLen() const { return master_.load(std::memory_order_relaxed); }
    uint32_t transportFrame() const { return tPub_.load(std::memory_order_relaxed); }
    // MIDI real-time bytes the audio thread made (clock out): an ISR hook for a
    // port that takes bytes there (DIN), and a ring for the main loop (USB).
    void setRealtimeHook(void (*fn)(uint8_t)) { rtHook_ = fn; }
    bool popRealtime(uint8_t& b);   // main loop
    uint32_t beatFrames() const { return beatLen(); }   // a beat, loop frames
    bool running() const { return runPub_.load(std::memory_order_relaxed); }
    bool monitoring() const { return monitor_; }
    uint8_t volume(int t) const { return trk_[t].volume; }
    PlayMode mode(int t) const { return PlayMode(view_[t].mode.load(std::memory_order_relaxed)); }
    uint8_t feedback() const { return feedback_; }
    uint8_t inputLevel(int ch) const { return inPeak_[ch].load(std::memory_order_relaxed); }
    uint8_t outputLevel(int ch) const { return outPeak_[ch].load(std::memory_order_relaxed); }
    // Mixer (audio-thread values; one byte each, so the main loop reads them as they are).
    uint8_t mix(MixParam p, int track = 0) const;
    uint8_t returnLevel(int slot) const { return retPeak_[slot].load(std::memory_order_relaxed); }
    uint8_t opt(OptParam p, int track = 0) const;
    // Bar length in frames: the master loop's bar once there is one, else from the tempo.
    uint32_t barFrames() const { return bar_.load(std::memory_order_relaxed); }
    uint8_t beatsPerBar() const { return beats_; }
    // FX slots and performance knobs (one byte each, read as they are).
    FxType fxType(int slot) const { return fx_[slot].type(); }
    uint8_t fxKnob(int slot, int i) const { return fx_[slot].knob(i); }
    bool fxBusy(int slot) const { return fx_[slot].busy(); }
    float fxLfo(int slot) const { return fx_[slot].lfo(); }
    uint8_t perfParam(PerfFx fx) const { return perfParam_[int(fx)]; }
    uint8_t grainParam(int t, int knob) const { return grain_[t][knob]; }
    // Granular mode, for the screen: the track's sounding grains (where each
    // reads now in the loop, its length, its envelope 0..1 now, its pan 0..1).
    struct LoopGrain {
        int32_t pos, width;
        float env, pan;
    };
    int loopGrains(int t, LoopGrain* out) const;   // out: kLoopGrains
    const CloudsFx& fxClouds(int slot) const { return fx_[slot].clouds(); }   // for the FX page picture
    // Tape tempo: the speed against the tempo the loops were recorded at.
    float speed() const { return float(speed16_) / 65536.0f; }
    float baseTempo() const;   // the recorded tempo (from the bar); the tempo now = baseTempo() * speed()

private:
    struct Grain {
        uint64_t pos16 = 0;          // read position, 16.16 frames
        uint32_t age = 0;            // output frames since the grain started
        bool back = false;           // reads backwards (Stretch in Reverse / Ping-pong's return)
    };
    // Zavalishin state-variable filter, one per channel.
    struct Svf {
        float ic1[2] = {0, 0}, ic2[2] = {0, 0};
    };
    struct Perf {
        uint8_t mask = 0;            // PerfFx bits
        uint32_t repStart = 0;       // repeat: slice start (buffer frames)
        uint32_t repSlice = 0;       //   slice length
        uint32_t repT0 = 0;          //   global frame of the press
        uint32_t repIn = 0;          //   position inside the slice at the press
        uint8_t repFx = 0xFF;        //   which repeat set the slice
        float tapePos = 0.0f;        // tape stop: read position (frames) and speed 1 -> 0
        uint32_t tapeBase = 0;
        float tapeRate = 1.0f;
        Osc vib;                     // vibrato LFO
        uint32_t lpT0 = 0, hpT0 = 0; // sweep starts
        float lpWet = 0.0f, hpWet = 0.0f;
        Svf lp, hp;
    };
    struct Track {
        TrackState state = TrackState::kClearing;
        PlayMode mode = PlayMode::kForward;
        bool muted = false;
        uint8_t volume = 80;
        float gain = 0.0f;           // smoothed (mute / volume ramps)
        float gate = 0.0f;           // smoothed mute / stop / stop fade, no volume: the pre-fader sends
        uint32_t anchor = 0;         // global frame where phase 0 starts (multiple of L)
        uint32_t len = 0;
        uint64_t offset16 = 0;       // phase offset (jumps, mode changes), 16.16
        uint32_t written = 0;        // high-water mark of frames touched since the last clear
        uint32_t recStart = 0;       // global frame of the recording start
        uint32_t stopAt = 0;         // recording ends when written reaches this (0 = by hand)
        bool mono = false;           // set from the options when recording starts
        InputCh input = InputCh::kStereo;
        Grain grain[2];
        clouds::Grain cg[kLoopGrains];   // Granular mode
        // A long take's grains: in the ring (else the RAM part), and where
        // each started in the buffer and in what it reads (for the screen).
        bool cgRing[kLoopGrains] = {};
        uint32_t cgB0[kLoopGrains] = {}, cgFirst[kLoopGrains] = {};
        float cgPhasor = 0.0f, cgNorm = 1.0f;
        Perf perf;
        uint32_t start = 0;          // buffer index of loop frame 0 (trim moves it)
        bool busy = false;           // an edit job is moving the audio
        bool launchDub = false;      // queued: overdub from the launch (Rec on a stopped track)
        uint32_t head = 0;           // a long take: loop frames from here on are on the card (StreamView)
        uint32_t fileUnits = 0;      //   its stream file holds this many (the whole take, from 0)
        bool cardUndo = false;       //   this overdub pass can be undone on the card
        // Recording / overdub at a changed tempo: the input resampled onto the
        // loop's own timeline (16.16 position, the input frame before).
        int64_t recPos16 = 0;        // recording: this input frame's place in the buffer, 16.16
        int16_t prevL = 0, prevR = 0;
        uint32_t tailTo = 0;         // still recording the loop-point tail up to this frame (0: no)
        uint32_t fadeOut = 0, fadeOutLen = 0;   // stopping with a fade: frames left, of
        bool replace = false;        // this overdub replaces what is there (punch-in)
        // Overdub undo: a session runs from an overdub's start to its end.
        bool dubSession = false;     // an overdub session is open (its cells are being saved)
        bool undoOk = false;         // the loop fits twice: this session can be undone
        bool undoUsed = false;       // cells were saved at the end of the buffer (a clear zeroes it all)
        uint8_t dubUndo = 0;         // 0 nothing, 1 can undo, 2 can redo (TrackView::dubUndo)
        float dubX = 0.0f;           // overdub / replace ramp, 0..1 (it ramps out after the stop too)
        uint32_t fadeAt = 0xFFFFFFFFu;   // the take's first frame after silence: it fades in from here
        uint32_t fxPos = 0xFFFFFFFFu;    // where a beat repeat or a tape stop read last (loop frame; the playhead shows it)
        // The fade-in gain for buffer frame i of a take (1 outside the fade).
        float fadeIn(uint32_t i) const {
            return i >= fadeAt && i - fadeAt < kFadeInFrames ? (float(i - fadeAt) + 0.5f) / float(kFadeInFrames) : 1.0f;
        }
        struct {
            bool has = false;
            uint32_t start = 0, len = 0, anchor = 0;
        } undo;                      // the loop before the last trim
    };
    struct Msg {
        uint8_t cmd, track;
        int16_t arg;
        uint32_t a, b;
    };
    // An edit that moves or zeroes audio: set up by the audio thread, run in
    // chunks by service() (main loop), finished by the audio thread.
    struct EditJob {
        std::atomic<uint8_t> state{0};   // 0 idle, 1 running (main loop), 2 done (audio thread)
        uint8_t track = 0, kind = 0;     // kind: kEditCut / kEditErase
        bool mono = false;
        uint32_t base = 0, len = 0;      // the loop in its buffer
        uint32_t a = 0, b = 0;           // selection, loop frames
        uint32_t done = 0;               // frames moved / zeroed so far
        uint32_t peaksDone = 0;          // then the peak cells, recomputed from the audio
    };
    void runJobChunk();
    void finishJob();

    void apply(const Msg& m);
    void startRecording(int t);
    void requestStop(int t);                 // a press: quantised stop, or now when free
    void finishNow(int t);                   // transport stop, memory full: whole units, padded
    void finishRecording(int t, uint32_t len);
    uint32_t barLen() const;
    uint32_t beatLen() const { return barLen() / beats_; }
    uint32_t capOf(int t) const { return trk_[t].mono ? 2 * mem_[t].capacity : mem_[t].capacity; }
    // Long takes: the ring, where the take moves to the card, and the longest take.
    uint32_t ringOf(int t) const { return trk_[t].mono ? 2 * kStreamRingFrames : kStreamRingFrames; }
    uint32_t blockOf(int t) const { return trk_[t].mono ? kStreamBlockBytes / 2 : kStreamBlockBytes / 4; }
    uint32_t headOf(int t) const {   // whole blocks of RAM before the ring
        return capOf(t) > 2 * ringOf(t) ? (capOf(t) - ringOf(t)) / blockOf(t) * blockOf(t) : 0;
    }
    void beginStream(int t);   // a take starts: mirror it on the card when a stream file is ready
    uint32_t recCap(int t) const;
    // Record loop frame i (into RAM or the ring; the first frame past headOf
    // makes a take long when a stream file is ready). False: the ring is full.
    bool recFrame(int t, uint32_t i, int16_t l, int16_t r, bool take, bool peaks);
    void endTake(int t);   // a long take's recording (and tail) is over: the Streamer pads and plays it
    // Where an overdub writes loop frame i: RAM, or a long take's ring slot
    // (marked for the Streamer to write back); null when the ring does not hold it.
    int16_t* dubFrame(int t, uint32_t i);
    void setOpt(OptParam p, int track, int value);
    void setPerf(int t, PerfFx fx, bool on);
    // Stop / launch: a stopped track starts again from its beginning on the
    // next bar line. Starting the clock for one track leaves the others stopped.
    void queueLaunch(int t, bool dub);
    void launch(Track& k);
    void startClockAlone(int t);
    void startTransport();   // as Play: everything that was playing, from bar 1
    void startRepeat(Track& k, PerfFx r);
    void beginTake(int t, uint32_t tf, const int16_t* pre, uint32_t preFrames);   // sound start
    void dubVari(int t, uint64_t ph, uint64_t step16, const int16_t* in, float oldK, float inK);   // overdub off speed 1
    void crossfadeLoopPoint(int t);   // blend the recorded tail into the loop's start
    void setLength(Track& k, uint32_t len);   // keeps the read position
    // Overdub undo: before the overdub writes loop frame i the first time this
    // session, save its cell. The undo copy of loop frame i is at undoBase + i.
    uint32_t undoBase(int t) const { return capOf(t) - trk_[t].len; }
    void saveUndoCell(int t, uint32_t i);
    void dropUndo(Track& k) { k.dubUndo = 0; k.dubSession = false; }
    uint8_t undoMap_[kTracks][kUndoMapBytes] = {};
    void clearTrack(int t);
    void setMode(int t, PlayMode mode);
    uint64_t period16(const Track& k) const;
    uint64_t phase16(const Track& k) const;           // current phase at t_
    uint32_t posOfPhase(const Track& k, uint64_t phase16) const;   // read position, frames
    uint64_t phaseOfPos(const Track& k, uint32_t pos, bool secondHalf) const;
    // What plays now: the read position, ahead of the clock by the latency.
    uint32_t posOf(const Track& k) const { return posOfPhase(k, readPhase16(k)); }
    // Input arrives the round-trip latency after the output it was played
    // against, so takes land that late in their buffers. Playback reads that
    // far ahead of the clock to put them back in time; an overdub writes at
    // the clock (that far behind the read position).
    uint64_t latShift16(const Track& k) const;   // the latency as phase, 16.16
    uint64_t readPhase16(const Track& k) const;  // phase16 + the latency
    void restartGrains(Track& k, uint64_t pos16);
    bool allEmpty() const;
    void publish();

    TrackMemory mem_[kTracks];
    Track trk_[kTracks];
    TrackView view_[kTracks];
    StreamView stream_[kTracks];

    // main loop -> audio thread
    static constexpr uint32_t kQueue = 256;   // a project load posts ~80 commands at once
    Msg queue_[kQueue];
    std::atomic<uint32_t> qHead_{0}, qTail_{0};

    // audio thread state
    // The bar clock runs in loop (buffer) frames, like tape: speed16_ loop
    // frames per output frame, 16.16 (the tempo against the recorded one).
    // t_ is its whole frames; tf(f) the clock at output frame f of the block.
    uint32_t t_ = 0;                 // global transport frame (loop frames)
    uint64_t tAcc_ = 0;              // the same, 16.16
    uint32_t speed16_ = 65536;       // 32768 .. 131072 (50 .. 200 %), 1.0 while there is no loop
    uint32_t lastBeat_ = 0xFFFFFFFFu;   // metronome: the beat of the last frame clicked
    void setClock(uint32_t frames) {
        t_ = frames;
        tAcc_ = uint64_t(frames) << 16;
        lastBeat_ = 0xFFFFFFFFu;
    }
    uint32_t tf(uint32_t f) const { return uint32_t((tAcc_ + uint64_t(f) * speed16_) >> 16); }
    bool canDubNow(const Track& k) const {   // at a changed tempo only Forward tape tracks overdub (resampled)
        return modeInfo(k.mode).canDub &&
               (speed16_ == 65536 || (k.mode == PlayMode::kForward && !opts_[&k - trk_].stretch));
    }
    uint32_t L_ = 0;                 // master loop length
    bool running_ = false;
    bool monitor_ = true;
    uint8_t feedback_ = 100;
    uint32_t lat_ = 0;               // round-trip latency compensation, output frames (kLatency)

    // Latency measurement: a click out every kMeasGap frames, timed back at
    // the input; the median of kMeasPings.
    static constexpr uint32_t kMeasGap = 13230, kMeasWindow = 4410;
    static constexpr int kMeasPings = 7;
    bool meas_ = false, measWait_ = false;
    uint32_t measClock_ = 0, measAt_ = 0;
    int measPing_ = 0, measHits_ = 0;
    uint16_t measGot_[kMeasPings] = {};
    std::atomic<uint8_t> measState_{0};
    std::atomic<uint16_t> measFrames_{0};
    void measure(int16_t* out, const int16_t* in, uint32_t n);

    // recording options, tempo and metronome (kept across track clears)
    TrackOpts opts_[kTracks];
    uint8_t bpm_ = 94, beats_ = 4;   // the tempo at power-on
    uint32_t bpm16_ = 0;                // a precise tempo from MIDI clock (16.16 bpm), 0: bpm_
    bool clockOut_ = false, clockRan_ = false;
    uint32_t lastTick_ = 0xFFFFFFFFu;
    void (*rtHook_)(uint8_t) = nullptr;
    uint8_t rt_[64] = {};
    std::atomic<uint32_t> rtHead_{0}, rtTail_{0};
    void pushRealtime(uint8_t b);
    bool metro_ = false;
    uint8_t click_ = 60;
    uint32_t clickAge_ = 0xFFFFFFFFu;   // frames since the last click started
    float clickAmp_ = 0.0f;
    Osc clickOsc_;
    std::atomic<uint32_t> bar_{0};
    uint8_t threshold_ = 50;            // -30 dBFS
    uint8_t stopFade_ = 0;              // OptParam::kStopFade
    uint8_t monitorFx_ = 0;             // OptParam::kMonitorFx
    int16_t preRoll_[kPreRollFrames * 2] = {};   // the input before this block, oldest first
    uint8_t perfParam_[kPerfFxCount] = {};
    uint8_t grain_[kTracks][kGrainKnobs] = {};
    float grainOut_[128 * 2], grainEnv_[128];   // Granular mode, one track at a time
    void renderGrains(int t, uint64_t ph0, uint64_t step16, uint32_t n);

    // mixer and send effects (kept across track clears)
    void setMix(MixParam p, int track, int value);
    uint8_t send_[kTracks][kFxSlots] = {};
    uint8_t return_[kFxSlots] = {80, 80};
    uint8_t masterLevel_ = 100;
    float sendGain_[kTracks][kFxSlots] = {};   // smoothed
    float returnGain_[kFxSlots] = {0.8f, 0.8f};
    uint8_t chain_[kFxSlots] = {0, 0};      // slot s's output into the other slot
    float chainGain_[kFxSlots] = {0.0f, 0.0f};
    float masterGain_ = 1.0f;
    FxUnit fx_[kFxSlots];
    float busL_[kFxSlots][128], busR_[kFxSlots][128];
    std::atomic<uint8_t> retPeak_[kFxSlots];

    // main loop zeroing (kClearing tracks)
    std::atomic<uint32_t> zeroTodo_[kTracks];
    // and a long take's peaks past the RAM's: bytes [from, to), to = 0 when done
    uint32_t zeroPeakFrom_[kTracks] = {};
    std::atomic<uint32_t> zeroPeakTo_[kTracks];
    EditJob job_;

    std::atomic<uint32_t> master_{0}, tPub_{0};
    std::atomic<bool> runPub_{false};
    std::atomic<uint8_t> inPeak_[2], outPeak_[2];
};

}  // namespace looper
