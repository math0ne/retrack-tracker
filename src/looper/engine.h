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

namespace looper {

constexpr int kTracks = 4;
constexpr int kFxSlots = 2;                  // FX 1, FX 2 (any FxType each), fed by per-track sends
constexpr int kSegments = 8;                 // pad columns 4..11 jump between eighths
constexpr uint32_t kPeakFrames = 256;        // frames per waveform peak cell
constexpr uint32_t kSampleRate = 44100;
constexpr uint32_t kGrainFrames = 2048;      // granular modes: 46 ms grains, 50 % overlap

enum class TrackState : uint8_t {
    kClearing, kEmpty, kCountIn, kArmed, kRecording, kPlaying, kOverdubbing, kStopped
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
};
enum class OptParam : uint8_t {
    kStop, kLength, kStart, kInput, kAfter,               // per track
    kTempo, kBeats, kMetronome, kClick, kThreshold,       // global: BPM 40..240, beats per bar 1..12,
                                                          // click on/off, level, sound threshold 0..100
};
constexpr uint8_t kMaxLenBeats = 64;
// Sound start: threshold 0..100 maps to -60..0 dBFS; the take keeps this much
// input from just before the trigger, so the attack is not cut.
inline float thresholdDb(uint8_t v) { return -60.0f + 0.6f * float(v > 100 ? 100 : v); }
constexpr uint32_t kPreRollFrames = 256;   // 5.8 ms

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
    kForward, kReverse, kPingPong, kHalfSpeed, kDoubleSpeed, kStretch, kOctaveUp, kOctaveDown, kCount
};
constexpr int kModeCount = int(PlayMode::kCount);

struct ModeInfo {
    enum Reader : uint8_t { kDirect, kTape, kGrains };
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
};
inline const ModeInfo& modeInfo(PlayMode m) { return kModes[int(m) < kModeCount ? int(m) : 0]; }

// Mixer parameters (Engine::kMix). Track parameters use the command's track field.
enum class MixParam : uint8_t {
    kLevel,          // track level 0..100 (same as kVolume)
    kSend1,          // track -> FX 1, post-fader, 0..100
    kSend2,          // track -> FX 2
    kReturn1,        // FX 1 return level 0..100
    kReturn2,
    kMaster,         // master level 0..100
};

// Effect memory the app hands to Engine::init (SDRAM on the device): a delay
// line per slot, read in order. Reverb lines, read at eight places per sample,
// are internal SRAM (engine.cpp): the SDRAM is uncached behind a 16-bit bus.
constexpr uint32_t kFxMemBytes = kFxSlots * Delay::kBytes;

struct TrackMemory {
    int16_t* frames = nullptr;   // interleaved stereo
    uint32_t capacity = 0;       // frames
    uint8_t* peaks = nullptr;    // capacity / kPeakFrames cells of 2 bytes (left, right), 0..255
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
};

class Engine {
public:
    enum Cmd : uint8_t {
        kRecord,       // empty -> record, record -> play, play <-> overdub, stopped -> overdub
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
    };

    // fxMem: kFxMemBytes, 4-byte aligned.
    void init(const TrackMemory mem[kTracks], uint8_t* fxMem);
    void post(Cmd c, uint8_t track = 0, int16_t arg = 0, uint32_t a = 0, uint32_t b = 0);   // main loop only
    void service();   // main loop: zero cleared buffers, run edit jobs
    bool editBusy() const { return job_.state.load(std::memory_order_acquire) != 0; }
    void render(int16_t* out, const int16_t* in, uint32_t frames);   // audio thread

    const TrackView& view(int t) const { return view_[t]; }
    const TrackMemory& memory(int t) const { return mem_[t]; }
    uint32_t masterLen() const { return master_.load(std::memory_order_relaxed); }
    uint32_t transportFrame() const { return tPub_.load(std::memory_order_relaxed); }
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

private:
    struct Grain {
        uint64_t pos16 = 0;          // read position, 16.16 frames
        uint32_t age = 0;            // output frames since the grain started
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
        uint32_t anchor = 0;         // global frame where phase 0 starts (multiple of L)
        uint32_t len = 0;
        uint64_t offset16 = 0;       // phase offset (jumps, mode changes), 16.16
        uint32_t written = 0;        // high-water mark of frames touched since the last clear
        uint32_t recStart = 0;       // global frame of the recording start
        uint32_t stopAt = 0;         // recording ends when written reaches this (0 = by hand)
        bool mono = false;           // set from the options when recording starts
        InputCh input = InputCh::kStereo;
        Grain grain[2];
        Perf perf;
        uint32_t start = 0;          // buffer index of loop frame 0 (trim moves it)
        bool busy = false;           // an edit job is moving the audio
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
    void setOpt(OptParam p, int track, int value);
    void setPerf(int t, PerfFx fx, bool on);
    void startRepeat(Track& k, PerfFx r);
    void beginTake(int t, uint32_t tf, const int16_t* pre, uint32_t preFrames);   // sound start
    void clearTrack(int t);
    void setMode(int t, PlayMode mode);
    uint64_t period16(const Track& k) const;
    uint64_t phase16(const Track& k) const;           // current phase at t_
    uint32_t posOfPhase(const Track& k, uint64_t phase16) const;   // read position, frames
    uint64_t phaseOfPos(const Track& k, uint32_t pos, bool secondHalf) const;
    uint32_t posOf(const Track& k) const { return posOfPhase(k, phase16(k)); }
    void restartGrains(Track& k, uint64_t pos16);
    bool allEmpty() const;
    void publish();

    TrackMemory mem_[kTracks];
    Track trk_[kTracks];
    TrackView view_[kTracks];

    // main loop -> audio thread
    static constexpr uint32_t kQueue = 64;
    Msg queue_[kQueue];
    std::atomic<uint32_t> qHead_{0}, qTail_{0};

    // audio thread state
    uint32_t t_ = 0;                 // global transport frame
    uint32_t L_ = 0;                 // master loop length
    bool running_ = false;
    bool monitor_ = true;
    uint8_t feedback_ = 100;

    // recording options, tempo and metronome (kept across track clears)
    TrackOpts opts_[kTracks];
    uint8_t bpm_ = 120, beats_ = 4;
    bool metro_ = false;
    uint8_t click_ = 60;
    uint32_t clickAge_ = 0xFFFFFFFFu;   // frames since the last click started
    float clickAmp_ = 0.0f;
    Osc clickOsc_;
    std::atomic<uint32_t> bar_{0};
    uint8_t threshold_ = 50;            // -30 dBFS
    int16_t preRoll_[kPreRollFrames * 2] = {};   // the input before this block, oldest first
    uint8_t perfParam_[kPerfFxCount] = {};

    // mixer and send effects (kept across track clears)
    void setMix(MixParam p, int track, int value);
    uint8_t send_[kTracks][kFxSlots] = {};
    uint8_t return_[kFxSlots] = {80, 80};
    uint8_t masterLevel_ = 100;
    float sendGain_[kTracks][kFxSlots] = {};   // smoothed
    float returnGain_[kFxSlots] = {0.8f, 0.8f};
    float masterGain_ = 1.0f;
    FxUnit fx_[kFxSlots];
    float busL_[kFxSlots][128], busR_[kFxSlots][128];
    std::atomic<uint8_t> retPeak_[kFxSlots];

    // main loop zeroing (kClearing tracks)
    std::atomic<uint32_t> zeroTodo_[kTracks];
    EditJob job_;

    std::atomic<uint32_t> master_{0}, tPub_{0};
    std::atomic<bool> runPub_{false};
    std::atomic<uint8_t> inPeak_[2], outPeak_[2];
};

}  // namespace looper
