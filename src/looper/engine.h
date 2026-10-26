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

namespace looper {

constexpr int kTracks = 4;
constexpr int kSegments = 8;                 // pad columns 4..11 jump between eighths
constexpr uint32_t kPeakFrames = 256;        // frames per waveform peak cell
constexpr uint32_t kSampleRate = 44100;
constexpr uint32_t kGrainFrames = 2048;      // granular modes: 46 ms grains, 50 % overlap

enum class TrackState : uint8_t { kClearing, kEmpty, kRecording, kPlaying, kOverdubbing, kStopped };

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
    };

    void init(const TrackMemory mem[kTracks]);
    void post(Cmd c, uint8_t track = 0, int16_t arg = 0);   // main loop only
    void service();                                          // main loop: zero cleared buffers
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

private:
    struct Grain {
        uint64_t pos16 = 0;          // read position, 16.16 frames
        uint32_t age = 0;            // output frames since the grain started
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
        Grain grain[2];
    };
    struct Msg {
        uint8_t cmd, track;
        int16_t arg;
    };

    void apply(const Msg& m);
    void startRecording(int t);
    void finishRecording(int t);
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

    // main loop zeroing (kClearing tracks)
    std::atomic<uint32_t> zeroTodo_[kTracks];

    std::atomic<uint32_t> master_{0}, tPub_{0};
    std::atomic<bool> runPub_{false};
    std::atomic<uint8_t> inPeak_[2], outPeak_[2];
};

}  // namespace looper
