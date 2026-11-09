// The sound board (S.Play): four WAVs from the card, on the top row's first
// four pads, played over the loops one at a time. Each slot keeps its start
// in SDRAM, so a pad sounds the moment it is pressed; the rest streams from
// the card into a ring per voice, read ahead by service() (main loop), a
// chunk at a time. Two voices: the one playing, and the one before it fading
// out (a new press stops what plays), so a change never clicks.
//
// Any PCM WAV plays: 8, 16, 24 or 32-bit or 32-bit float, mono or stereo (or
// the first two channels of more), at any rate (resampled to 44.1 kHz). The
// head and the rings hold the file's own bytes; render() converts them.
//
// The sample loader previews a WAV through it too (preview()): straight from
// the card, to the output only (never into a take).
//
// The board (which WAV is on which pad, the play mode, the level, whether
// takes record it, and its mixer strip: the sends and the DJ filter) is kept
// in Looper/soundboard.txt; the slot starts load from the card in the
// background at start-up.
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
    static constexpr uint32_t kHeadBytes = 176400;       // per slot, in SDRAM: 1 s of 16-bit stereo at 44.1 kHz
    static constexpr uint32_t kRingBytes = 64 * 1024;    // per voice: the rest, read ahead
    static constexpr uint32_t kChunkBytes = 16 * 1024;   // one card read (at most; a whole number of frames)
    static constexpr int kPathLen = 96;
    static constexpr int kNameLen = 24;
    enum class Play : uint8_t { kOneShot, kToggle, kGate };
    enum class SlotState : uint8_t { kEmpty, kPending, kReady, kMissing };
    // Why a slot is kMissing (whyText()).
    enum Why : uint8_t { kOk, kNoFile, kNotWav, kCompressed, kBits, kChannels, kRate, kNoSound, kCardError };

    struct Slot {
        char path[kPathLen];
        char name[kNameLen];   // the file name without .wav
        uint32_t dataOff;      // where the samples start in the file
        uint32_t frames;       // at the file's rate
        uint32_t headFrames;   // how many of them are in SDRAM
        uint32_t rate;
        uint8_t mono, sampleBytes, frameBytes, isFloat;
        SlotState state;
        uint8_t why;           // Why, when kMissing
        uint8_t pad[2];
    };
    static constexpr uint32_t kSlotBytes = (uint32_t((kSlots + 1) * sizeof(Slot)) + 15u) & ~15u;   // + the preview
    static constexpr uint32_t kScratchBytes = 4096;   // the engine's source blocks (Engine::setSource)
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
    uint32_t length(int slot) const;           // in output frames (44.1 kHz)
    static const char* whyText(uint8_t why);
    float progress(int slot) const;            // 0..1 while it plays, < 0 when it does not
    int playing() const;                       // voices sounding
    Play mode() const { return mode_; }
    void setMode(Play m);
    int level() const { return level_.load(std::memory_order_relaxed); }
    void setLevel(int v);
    bool toLoops() const { return toLoops_; }   // takes record the sound board (it goes into the input)
    void setToLoops(bool on);
    // Its mixer strip (the engine runs them: Engine::setSourceMix).
    int send(int s) const { return send_[s]; }  // to FX 1 / 2, 0..100
    void setSend(int s, int v);
    int filter() const { return filter_; }      // DJ filter, 0..kFilterFineMax (the middle: off)
    void setFilter(int v);
    bool loading() const;                      // slot starts still to read
    const char* status() const { return status_; }
    uint32_t underruns() const { return underruns_.load(std::memory_order_relaxed); }

    // The sample loader's preview: the WAV from the card, to the output.
    bool preview(const char* path);            // false: not one it plays (status() says why)
    void stopPreview();
    bool previewing() const;

    // Audio thread: adds the pads into l / r and the preview into ol / orr;
    // false when nothing sounds.
    bool render(float* l, float* r, float* ol, float* orr, uint32_t n);

private:
    static constexpr int kPreview = kSlots;   // the preview's slot (not kept in the board file)
    struct Play_ {   // one play, as the audio thread needs it
        const uint8_t* head;
        const uint8_t* ring;
        uint32_t headFrames, frames, ringFrames;
        uint32_t step16;   // file frames per output frame, 16.16
        uint8_t mono, slot, sampleBytes, frameBytes, isFloat;
    };
    struct Voice {
        Play_ next = {};                        // main: the play to start, then gen++
        std::atomic<uint32_t> gen{0};           // main: starts
        std::atomic<uint32_t> stopGen{0};       // main: stop (fade out) the play of this gen
        std::atomic<uint32_t> avail{0};         // main: frames in the ring (after the head)
        std::atomic<uint32_t> ackGen{0};        // audio: the play it has taken
        std::atomic<uint32_t> pos{0};           // audio: file frames played
        std::atomic<bool> active{false};        // audio: sounding
        // audio thread only
        Play_ cur = {};
        uint32_t frac = 0;                      // between file frames, 16.16
        float fade = 1.0f;
        // main loop only
        ot::IStorage::Handle h = -1;
        uint32_t written = 0, left = 0;         // ring frames written, file frames still to read
        int slot = -1;
        uint32_t order = 0;                     // when it started (the oldest is replaced first)
    };
    static uint32_t chunkFrames(uint32_t fb) { return kChunkBytes / fb; }
    static uint32_t ringFrames(uint32_t fb) { return 4 * chunkFrames(fb); }   // a read never wraps
    bool cardOk() const;
    bool sounding(const Voice& v) const;       // started or still playing
    void start(int slot);
    void closeVoice(Voice& v);
    bool feed(Voice& v);                       // one card read into its ring; false: nothing to do
    bool openSlot(int slot, bool head);        // the WAV's format (and its start into SDRAM)
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
    uint8_t send_[2] = {0, 0};
    uint16_t filter_ = 500;
    bool boardLoaded_ = false, wasReady_ = false;
    bool dirty_ = false;          // the board file is written a second after the last change
    uint32_t dirtyMs_ = 0;
    void markDirty();
    std::atomic<uint32_t> underruns_{0};
    char status_[64] = {};
};

}  // namespace looper
