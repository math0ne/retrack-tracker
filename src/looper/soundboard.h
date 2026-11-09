// The sound board (S.Play): four WAVs from the card, on the top row's first
// four pads, played over the loops one at a time. Each slot keeps its first
// second in SDRAM, so a pad sounds the moment it is pressed; the rest streams
// from the card into a ring per voice, read ahead by service() (main loop),
// a chunk at a time. Two voices: the one playing, and the one before it
// fading out (a new press stops what plays), so a change never clicks.
//
// The board (which WAV is on which pad, the play mode, the level and whether
// takes record it) is kept in Looper/soundboard.txt; the slot starts load
// from the card in the background at start-up.
//
// Threads: press / release / stop and service() run in the main loop,
// render() in the audio callback. A voice start is published with a
// generation counter (Voice::gen); the audio thread takes the new play at its
// next block and acknowledges it (ackGen). The main loop never rewrites a
// start the audio thread has not taken yet.
#pragma once

#include <atomic>
#include <cstdint>

#include "hal_api.h"
#include "looper/platform.h"

namespace looper {

class Soundboard {
public:
    static constexpr int kSlots = 4;                     // pads 1..4 of the top row
    static constexpr int kVoices = 2;                    // the sound playing and the last one's fade
    static constexpr uint32_t kHeadBytes = 176400;       // per slot, in SDRAM: 1 s stereo, 2 s mono
    static constexpr uint32_t kRingBytes = 64 * 1024;    // per voice: the rest, read ahead
    static constexpr uint32_t kChunkBytes = 16 * 1024;   // one card read
    static constexpr int kPathLen = 96;
    static constexpr int kNameLen = 24;
    enum class Play : uint8_t { kOneShot, kToggle, kGate };
    enum class SlotState : uint8_t { kEmpty, kPending, kReady, kMissing };

    struct Slot {
        char path[kPathLen];
        char name[kNameLen];   // the file name without .wav
        uint32_t dataOff;      // where the samples start in the file
        uint32_t frames;
        uint32_t headFrames;   // how many of them are in SDRAM
        uint8_t mono;
        SlotState state;
        uint8_t pad[2];
    };
    static constexpr uint32_t kSlotBytes = (uint32_t(kSlots * sizeof(Slot)) + 15u) & ~15u;
    static constexpr uint32_t kScratchBytes = 2048;   // the engine's source blocks (Engine::setSource)
    // SDRAM: the slot starts, the voice rings, the slot table, the scratch.
    static constexpr uint32_t kMemBytes = kSlots * kHeadBytes + kVoices * kRingBytes + kSlotBytes + kScratchBytes;
    uint8_t* scratch() const { return mem_ + (kMemBytes - kScratchBytes); }

    void init(const Platform* p, uint8_t* mem);   // mem: kMemBytes, 16-byte aligned
    void service();                               // main loop

    // Main loop
    bool assign(int slot, const char* path);   // false: not a WAV this plays (status() says why)
    void clear(int slot);
    void press(int slot);                      // as the play mode says
    void release(int slot);                    // Gate: stops it
    void stop(int slot);
    void stopAll();
    const Slot& slot(int i) const { return slots_[i]; }
    float progress(int slot) const;            // 0..1 while it plays, < 0 when it does not
    int playing() const;                       // voices sounding
    Play mode() const { return mode_; }
    void setMode(Play m);
    int level() const { return level_.load(std::memory_order_relaxed); }
    void setLevel(int v);
    bool toLoops() const { return toLoops_; }   // takes record the sound board (it goes into the input)
    void setToLoops(bool on);
    bool loading() const;                      // slot starts still to read
    const char* status() const { return status_; }
    uint32_t underruns() const { return underruns_.load(std::memory_order_relaxed); }

    // Audio thread: adds the voices into l / r; false when none sounds.
    bool render(float* l, float* r, uint32_t n);

private:
    struct Play_ {   // one play, as the audio thread needs it
        const int16_t* head;
        const int16_t* ring;
        uint32_t headFrames, frames, ringFrames;
        uint8_t mono, slot;
    };
    struct Voice {
        Play_ next = {};                        // main: the play to start, then gen++
        std::atomic<uint32_t> gen{0};           // main: starts
        std::atomic<uint32_t> stopGen{0};       // main: stop (fade out) the play of this gen
        std::atomic<uint32_t> avail{0};         // main: frames in the ring (after the head)
        std::atomic<uint32_t> ackGen{0};        // audio: the play it has taken
        std::atomic<uint32_t> pos{0};           // audio: frames played
        std::atomic<bool> active{false};        // audio: sounding
        // audio thread only
        Play_ cur = {};
        float fade = 1.0f;
        // main loop only
        ot::IStorage::Handle h = -1;
        uint32_t written = 0, left = 0;         // ring frames written, file frames still to read
        int slot = -1;
        uint32_t order = 0;                     // when it started (the oldest is replaced first)
    };
    bool cardOk() const;
    bool sounding(const Voice& v) const;       // started or still playing
    void start(int slot);
    void closeVoice(Voice& v);
    bool feed(Voice& v);                       // one card read into its ring; false: nothing to do
    bool loadHead(int slot);                   // the slot's start into SDRAM
    void loadBoard();
    void saveBoard();
    uint8_t* headOf(int slot) const { return mem_ + size_t(slot) * kHeadBytes; }
    uint8_t* ringOf(int v) const { return mem_ + size_t(kSlots) * kHeadBytes + size_t(v) * kRingBytes; }

    const Platform* p_ = nullptr;
    uint8_t* mem_ = nullptr;
    Slot* slots_ = nullptr;
    Voice voices_[kVoices];
    uint32_t order_ = 0;
    Play mode_ = Play::kOneShot;
    std::atomic<int> level_{80};
    bool toLoops_ = true;
    bool boardLoaded_ = false, wasReady_ = false;
    bool dirty_ = false;          // the board file is written a second after the last change
    uint32_t dirtyMs_ = 0;
    void markDirty();
    std::atomic<uint32_t> underruns_{0};
    char status_[64] = {};
};

}  // namespace looper
