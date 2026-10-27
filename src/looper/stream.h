// Takes on the SD card, the main loop's half (docs/SD_STREAMING.md; the audio
// thread's half is in Engine: StreamView, recFrame, dubFrame).
//
// Once the card is ready the Streamer allocates two stream files per track
// (Looper/Stream/trackN.raw and trackN.b.raw, kStreamFrames of stereo each,
// contiguous: f_expand) and tells the engine (StreamView::fileFrames,
// undoFiles). Every take is then mirrored to file A as it records. One that
// outgrows its RAM (past `head`) goes on into a ring at the end of the
// track's RAM; service() drains it to the file, pads the file with zeros to
// the loop's length, then keeps the ring filled around the playhead
// (kStreamBehind behind it, the rest ahead).
//
// The files are kept in 32 KB blocks; a small map says which file holds each
// block's current version. An overdub marks the blocks it changes (in the RAM
// part and in the ring); a changed block goes back to the card once the
// playhead has left it, and its first write in a pass goes to the other file,
// so the pass before stays there: undo flips those blocks back (redo again),
// reloads the RAM part's and recomputes their peaks. Each call does at most
// one 32 KB card command per track, with direct DMA to and from the SDRAM
// (Platform::sdDirectDma) when the platform has it.
#pragma once

#include <cstdint>

#include "hal_api.h"
#include "looper/engine.h"
#include "looper/platform.h"

namespace looper {

class Streamer {
public:
    static constexpr uint32_t kChunkBytes = kStreamBlockBytes;   // a card command: one block

    void init(Engine* engine, const Platform* p);
    void service();                  // main loop
    bool active() const;             // a long take is on the card (or going there)
    bool ready(int t) const { return hA_[t] >= 0; }   // track t's stream file is open
    const char* status() const { return status_; }   // the file setup's problem, "" when fine
    uint32_t errors() const { return errors_; }      // card commands that failed
    // For the File jobs (main loop): n units of track t from buffer frame b,
    // as it plays now (the RAM part, the ring where an overdub changed it and
    // it is not back yet, the card); and, for a project load into an empty
    // track, into its stream file A. False: no file, or a card error.
    bool read(int t, uint32_t b, uint8_t* dst, uint32_t n);
    bool write(int t, bool mono, uint32_t b, const uint8_t* src, uint32_t n);
    // Track t's stream file can take a loop of this many units (0: none).
    uint32_t capacity(int t, bool mono) const { return ready(t) ? (mono ? 2 : 1) * kStreamFrames : 0; }

private:
    static constexpr uint32_t kWords = StreamView::kBlockWords;
    // One track's take, worked out once per call.
    struct Ctx {
        StreamView* sv;
        uint8_t* ram;        // the track's RAM (loop frame 0)
        uint32_t ub;         // bytes per unit (frame or mono sample)
        uint32_t bu;         // units per block
        uint32_t head, ring;
        uint32_t len;        // the end of the loop in the buffer (start + length): the file mirrors the buffer
        uint8_t* slot(uint32_t i) const { return ram + size_t(head + (i - head) % ring) * ub; }
        uint8_t* at(uint32_t i) const { return i < head || !head ? ram + size_t(i) * ub : slot(i); }
    };
    bool cardOk() const;
    void setUp();          // one stream file per call
    void closeAll();
    bool serviceTrack(int t);
    bool drain(int t, const Ctx& c);
    // A changed block of pass parity par back to the card; true when a card
    // command ran or must run again (a failed write stays marked).
    bool writeBlock(int t, const Ctx& c, int par, uint32_t b);
    bool writeBack(int t, const Ctx& c, uint32_t pos, bool all, uint32_t lo, uint32_t hi);
    bool undoStep(int t, const Ctx& c);
    bool editStep(int t, const Ctx& c);   // a cut or erase on the card
    bool flushAll(int t, const Ctx& c);   // every changed block back (the ring stops being used)
    void peaks(int t, const Ctx& c, uint32_t b, const uint8_t* data);   // recompute one block's
    bool io(bool write, ot::IStorage::Handle h, uint32_t byteOff, uint8_t* buf, uint32_t bytes);
    bool bit(const uint32_t* m, uint32_t b) const { return (m[b >> 5] >> (b & 31)) & 1u; }
    ot::IStorage::Handle fileOf(int t, uint32_t b) const { return bit(cur_[t], b) ? hB_[t] : hA_[t]; }

    Engine* e_ = nullptr;
    const Platform* p_ = nullptr;
    ot::IStorage::Handle hA_[kTracks] = {-1, -1, -1, -1}, hB_[kTracks] = {-1, -1, -1, -1};
    bool tried_[kTracks] = {}, triedB_[kTracks] = {};
    // Per track: which file holds each block now (1: B), which blocks this
    // overdub pass has moved to the other file, the pass they belong to.
    // sess_ per pass parity: the pass now (session_) and the one before.
    uint32_t cur_[kTracks][kWords] = {}, sess_[kTracks][2][kWords] = {};
    uint32_t session_[kTracks] = {};
    uint32_t take_[kTracks] = {};   // the take the maps below belong to (StreamView::take)
    // The ring is being moved: its changed blocks in [pendS_, pendE_) go back first.
    bool pend_[kTracks] = {};
    uint32_t pendS_[kTracks] = {}, pendE_[kTracks] = {};
    uint8_t undoPhase_[kTracks] = {};   // 0 none, 1 writing back, 2 reloading
    uint32_t undoNext_[kTracks] = {};
    uint8_t editPhase_[kTracks] = {};   // 0 none, 1 writing back, 2 moving / zeroing, 3 peaks
    uint32_t editNext_[kTracks] = {};   // the next unit (moving / zeroing) or block (peaks)
    uint32_t editStart_[kTracks] = {}, editLen_[kTracks] = {};   // the loop in its buffer at the request
    uint32_t errors_ = 0;
    int next_ = 0;   // round robin over the tracks
    char status_[64] = {};
};

}  // namespace looper
