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
// anchored to a multiple of L and its length is rounded up to a multiple of L,
// so idx = (t - anchor + offset) mod len stays bar-aligned for all tracks.
#pragma once

#include <atomic>
#include <cstdint>

namespace looper {

constexpr int kTracks = 4;
constexpr int kSegments = 8;                 // pad columns 4..11 jump between eighths
constexpr uint32_t kPeakFrames = 256;        // frames per waveform peak cell
constexpr uint32_t kSampleRate = 44100;

enum class TrackState : uint8_t { kClearing, kEmpty, kRecording, kPlaying, kOverdubbing, kStopped };

struct TrackMemory {
    int16_t* frames = nullptr;   // interleaved stereo
    uint32_t capacity = 0;       // frames
    uint8_t* peaks = nullptr;    // capacity / kPeakFrames cells, 0..255
};

// Snapshot the UI reads; written by the audio thread only.
struct TrackView {
    std::atomic<uint8_t> state{uint8_t(TrackState::kClearing)};
    std::atomic<bool> muted{false};
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
    uint8_t feedback() const { return feedback_; }
    uint8_t inputLevel(int ch) const { return inPeak_[ch].load(std::memory_order_relaxed); }
    uint8_t outputLevel(int ch) const { return outPeak_[ch].load(std::memory_order_relaxed); }

private:
    struct Track {
        TrackState state = TrackState::kClearing;
        bool muted = false;
        uint8_t volume = 80;
        float gain = 0.0f;           // smoothed (mute / volume ramps)
        uint32_t anchor = 0;         // global frame where idx 0 starts (multiple of L)
        uint32_t len = 0;
        uint32_t offset = 0;         // jump offset, multiple of len / kSegments
        uint32_t written = 0;        // high-water mark of frames touched since the last clear
        uint32_t recStart = 0;       // global frame of the recording start
    };
    struct Msg {
        uint8_t cmd, track;
        int16_t arg;
    };

    void apply(const Msg& m);
    void startRecording(int t);
    void finishRecording(int t);
    void clearTrack(int t);
    uint32_t indexOf(const Track& k) const;
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
