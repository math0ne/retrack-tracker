#include "looper/stream.h"

#include <cstdio>
#include <cstring>

namespace looper {

using ot::IStorage;

namespace {
constexpr char kDir[] = "/Looper/Stream";
constexpr uint32_t kFileBytes = kStreamFrames * 4u;
constexpr uint32_t kMargin = 2048;   // units: a changed block waits until the playhead is this far away

inline uint8_t peak8(int v) {
    if (v < 0) v = -v;
    return uint8_t(v >> 7);
}
}  // namespace

void Streamer::init(Engine* engine, const Platform* p) {
    e_ = engine;
    p_ = p;
}

bool Streamer::cardOk() const {
    return p_ && p_->storage && (!p_->storageReady || p_->storageReady());
}

bool Streamer::active() const {
    for (int t = 0; t < kTracks; ++t)
        if (e_->longTake(t)) return true;
    return false;
}

void Streamer::closeAll() {
    for (int t = 0; t < kTracks; ++t) {
        StreamView& sv = e_->stream(t);
        sv.fileFrames.store(0, std::memory_order_release);
        sv.undoFiles.store(false, std::memory_order_release);
        if (cardOk()) {
            if (hA_[t] >= 0) p_->storage->close(hA_[t]);
            if (hB_[t] >= 0) p_->storage->close(hB_[t]);
        }
        hA_[t] = hB_[t] = -1;
        tried_[t] = triedB_[t] = false;
    }
}

void Streamer::setUp() {
    // The first missing file: allocate it (or keep it), open it. A first, then B.
    int t = 0;
    bool b = false;
    while (t < kTracks && (hA_[t] >= 0 || tried_[t])) ++t;
    if (t == kTracks) {
        t = 0;
        while (t < kTracks && (hB_[t] >= 0 || triedB_[t] || hA_[t] < 0)) ++t;
        if (t == kTracks) return;
        b = true;
    }
    (b ? triedB_ : tried_)[t] = true;
    IStorage& fs = *p_->storage;
    fs.mkdir("/Looper");
    fs.mkdir(kDir);
    char path[40];
    std::snprintf(path, sizeof path, "%s/track%d%s.raw", kDir, t + 1, b ? ".b" : "");
    if (!fs.preallocate(path, kFileBytes)) {
        std::snprintf(status_, sizeof status_, "No room for the stream files (%lu MB each)",
                      static_cast<unsigned long>(kFileBytes >> 20));
        return;
    }
    const IStorage::Handle h = fs.open(path, IStorage::kReadWrite);
    if (h < 0) {
        std::snprintf(status_, sizeof status_, "Could not open %s", path);
        return;
    }
    fs.fastSeek(h);   // write-back and read-ahead seek back and forth
    if (b) {
        hB_[t] = h;
        e_->stream(t).undoFiles.store(true, std::memory_order_release);
    } else {
        hA_[t] = h;
        e_->stream(t).fileFrames.store(kStreamFrames, std::memory_order_release);
    }
}

bool Streamer::io(bool write, IStorage::Handle h, uint32_t byteOff, uint8_t* buf, uint32_t bytes) {
    if (h < 0) return false;
    IStorage& fs = *p_->storage;
    const bool dma = p_->sdDirectDma && p_->sdDirectDma(true);
    bool ok = fs.seek(h, byteOff);
    if (ok) ok = (write ? fs.write(h, buf, bytes) : fs.read(h, buf, bytes)) == int32_t(bytes);
    if (dma) p_->sdDirectDma(false);
    if (!ok) ++errors_;
    return ok;
}

void Streamer::peaks(int t, const Ctx& c, uint32_t b, const uint8_t* data) {
    // The waveform cells of block b from its audio (after an undo / redo).
    const TrackMemory& m = e_->memory(t);
    const bool mono = c.ub == 2;
    const uint32_t f0 = b * c.bu, f1 = f0 + c.bu < c.len ? f0 + c.bu : c.len;
    const int16_t* s = reinterpret_cast<const int16_t*>(data);
    for (uint32_t cell = f0 / kPeakFrames; cell * kPeakFrames < f1; ++cell) {
        uint8_t pl = 0, pr = 0;
        for (uint32_t f = cell * kPeakFrames; f < (cell + 1) * kPeakFrames && f < f1; ++f) {
            const uint32_t j = f - f0;
            const uint8_t a = peak8(mono ? s[j] : s[2 * j]), bb = mono ? a : peak8(s[2 * j + 1]);
            if (a > pl) pl = a;
            if (bb > pr) pr = bb;
        }
        if (mono) {
            if (cell < m.peakBytes) m.peaks[cell] = pl;
        } else if (2 * cell + 1 < m.peakBytes) {
            m.peaks[2 * cell] = pl;
            m.peaks[2 * cell + 1] = pr;
        }
    }
}

bool Streamer::drain(int t, const Ctx& c) {
    // Recording (and the zeros to the loop's end after): the take goes to file A.
    StreamView& sv = *c.sv;
    const bool rec = sv.recording.load(std::memory_order_acquire);
    const uint32_t len = rec ? 0 : sv.len.load(std::memory_order_relaxed);
    const uint32_t recd = sv.recorded.load(std::memory_order_acquire);
    const uint32_t data = rec ? recd : (recd < len ? recd : len);
    const uint32_t target = rec ? data : len;
    const uint32_t fl = sv.flushed.load(std::memory_order_relaxed);
    if (fl >= target) return false;
    uint32_t n;
    uint8_t* src;
    if (fl < data) {
        n = data - fl;
        if (n > c.bu) n = c.bu;
        // Whole blocks while it is still coming: one 32 KB command, not a
        // stream of small ones (slow on a card, and each blocks the main loop).
        if (rec && n < c.bu) return false;
        if (c.head && fl < c.head && fl + n > c.head) n = c.head - fl;   // RAM, then the ring
        if (c.head && fl >= c.head && n > c.ring - (fl - c.head) % c.ring) n = c.ring - (fl - c.head) % c.ring;
        src = c.at(fl);
    } else {   // the silent rest of the last bar: zeros (the waveform staging buffer, here free)
        n = target - fl;
        if (n > c.bu) n = c.bu;
        src = p_->scratch;
        std::memset(src, 0, size_t(n) * c.ub);
    }
    if (!n) return false;
    if (!io(true, hA_[t], fl * c.ub, src, n * c.ub)) return false;
    sv.flushed.store(fl + n, std::memory_order_release);
    return true;
}

bool Streamer::writeBlock(int t, const Ctx& c, uint32_t b) {
    // Clear, then write: an overdub that changes it again meanwhile marks it again.
    StreamView& sv = *c.sv;
    sv.dirty[b >> 5].fetch_and(~(1u << (b & 31)), std::memory_order_relaxed);
    const uint32_t f0 = b * c.bu, n = f0 + c.bu < c.len ? c.bu : c.len - f0;
    if (!sv.undoFiles.load(std::memory_order_relaxed) || hB_[t] < 0)   // no second file: in place
        return io(true, hA_[t], f0 * c.ub, c.at(f0), n * c.ub);
    // The first write of the pass goes to the other file: the pass before stays.
    const bool moved = bit(sess_[t], b);
    const IStorage::Handle h = moved ? fileOf(t, b) : (bit(cur_[t], b) ? hA_[t] : hB_[t]);
    if (!io(true, h, f0 * c.ub, c.at(f0), n * c.ub)) return false;
    if (!moved) {
        sess_[t][b >> 5] |= 1u << (b & 31);
        cur_[t][b >> 5] ^= 1u << (b & 31);
    }
    return true;
}

bool Streamer::writeBack(int t, const Ctx& c, uint32_t pos, bool all, uint32_t lo, uint32_t hi) {
    // The first changed block in loop frames [lo, hi) the playhead has left (all:
    // any), ring blocks only while the ring holds them whole.
    StreamView& sv = *c.sv;
    if (hi <= lo) return false;
    const uint32_t rS = sv.rStart.load(std::memory_order_relaxed), rE = sv.rEnd.load(std::memory_order_relaxed);
    const uint32_t b0 = lo / c.bu, b1 = (hi + c.bu - 1) / c.bu;
    for (uint32_t w = b0 >> 5; w <= (b1 - 1) >> 5 && w < kWords; ++w) {
        uint32_t bits = sv.dirty[w].load(std::memory_order_relaxed);
        while (bits) {
            const uint32_t b = (w << 5) + uint32_t(__builtin_ctz(bits));
            bits &= bits - 1;
            if (b < b0 || b >= b1) continue;
            const uint32_t f0 = b * c.bu, f1 = f0 + c.bu;
            if (!all && pos + kMargin >= f0 && pos < f1 + kMargin) continue;   // the playhead is in it
            if (f0 >= c.head && !all && (f0 < rS || (f1 < c.len ? f1 : c.len) > rE)) continue;   // not in the ring
            return writeBlock(t, c, b);
        }
    }
    return false;
}

bool Streamer::undoStep(int t, const Ctx& c) {
    // Undo / redo a long take's last pass (the track is silent meanwhile):
    // every changed block back to the card, flip the pass's blocks to the
    // other file, reload the RAM part's, recompute their peaks, refill the ring.
    StreamView& sv = *c.sv;
    if (undoPhase_[t] == 0) {
        pendS_[t] = sv.rStart.load(std::memory_order_relaxed);
        pendE_[t] = sv.rEnd.load(std::memory_order_relaxed);
        undoPhase_[t] = 1;
    }
    if (undoPhase_[t] == 1) {
        if (flushAll(t, c)) return true;
        for (uint32_t w = 0; w < kWords; ++w) cur_[t][w] ^= sess_[t][w];
        undoNext_[t] = 0;
        undoPhase_[t] = 2;
        return true;
    }
    // Reload: one block of the pass a call.
    const uint32_t nb = (c.len + c.bu - 1) / c.bu;
    uint32_t b = undoNext_[t];
    while (b < nb && !bit(sess_[t], b)) ++b;
    if (b < nb) {
        undoNext_[t] = b + 1;
        const uint32_t f0 = b * c.bu, n = f0 + c.bu < c.len ? c.bu : c.len - f0;
        uint8_t* dst = f0 < c.head ? c.ram + size_t(f0) * c.ub : p_->scratch;   // the card part: just its peaks
        if (io(false, fileOf(t, b), f0 * c.ub, dst, n * c.ub)) peaks(t, c, b, dst);
        return true;
    }
    sv.rStart.store(c.head, std::memory_order_release);   // the ring refills from the files
    sv.rEnd.store(c.head, std::memory_order_release);
    undoPhase_[t] = 0;
    sv.undoDone.store(true, std::memory_order_release);
    return true;
}

bool Streamer::flushAll(int t, const Ctx& c) {
    // The ring's changed blocks while it still holds them, then the RAM part's;
    // false once nothing is left (the ring is then empty).
    StreamView& sv = *c.sv;
    if (writeBack(t, c, 0, true, pendS_[t], pendE_[t])) return true;
    if (writeBack(t, c, 0, true, 0, c.head)) return true;
    sv.rEnd.store(sv.rStart.load(std::memory_order_relaxed), std::memory_order_release);
    return false;
}

bool Streamer::editStep(int t, const Ctx& c) {
    // A long take's cut (the rest moves up over [A, B)) or erase (zeros over it),
    // in buffer frames A = start + a, B = start + b. RAM part in RAM (and its
    // card copy), the card part on the card through the staging buffer. Then
    // the peaks of what changed; then the ring refills.
    StreamView& sv = *c.sv;
    const bool cut = sv.editReq.load(std::memory_order_relaxed) == 1;
    if (editPhase_[t] == 0) {
        const TrackView& v = e_->view(t);
        editStart_[t] = v.start.load(std::memory_order_relaxed);
        editLen_[t] = v.len.load(std::memory_order_relaxed);
        pendS_[t] = sv.rStart.load(std::memory_order_relaxed);
        pendE_[t] = sv.rEnd.load(std::memory_order_relaxed);
        editNext_[t] = 0;
        editPhase_[t] = 1;
    }
    const uint32_t A = editStart_[t] + sv.editA, B = editStart_[t] + sv.editB, end = editStart_[t] + editLen_[t];
    const uint32_t bu = c.bu, ub = c.ub;
    Ctx d = c;   // the whole take, as it was, for the copy
    d.len = end;
    if (editPhase_[t] == 1) {
        if (flushAll(t, d)) return true;
        // From here on the files are written in place; the pass before is gone.
        std::memset(sess_[t], 0, sizeof sess_[t]);
        editPhase_[t] = 2;
        return true;
    }
    if (editPhase_[t] == 2) {
        uint8_t* const buf = p_->scratch;
        const uint32_t x = editNext_[t];
        if (cut) {   // [B + x, ...) to [A + x, ...): read through the staging buffer, then write
            if (B + x >= end) {
                std::memset(cur_[t], 0, sizeof cur_[t]);   // all of it is in file A now
                editNext_[t] = A / bu;
                editPhase_[t] = 3;
                return true;
            }
            uint32_t n = end - (B + x) < bu ? end - (B + x) : bu;
            const uint32_t dst = A + x;
            if (c.head && dst < c.head && dst + n > c.head) n = c.head - dst;   // RAM, then the card
            if (!read(t, B + x, buf, n)) return true;
            if (c.head && dst < c.head) std::memcpy(c.ram + size_t(dst) * ub, buf, size_t(n) * ub);
            io(true, hA_[t], dst * ub, buf, n * ub);
            editNext_[t] = x + n;
            return true;
        }
        // Erase: block by block over [A, B), in each block's file.
        const uint32_t f = A + x;
        if (f >= B) {
            editNext_[t] = A / bu;
            editPhase_[t] = 3;
            return true;
        }
        const uint32_t blk = f / bu, b0 = blk * bu, b1 = b0 + bu < end ? b0 + bu : end;
        const uint32_t z1 = B < b1 ? B : b1;
        if (c.head && b0 < c.head) {   // the RAM part: zero it there, then its card copy
            std::memset(c.ram + size_t(f) * ub, 0, size_t(z1 - f) * ub);
            io(true, fileOf(t, blk), b0 * ub, c.ram + size_t(b0) * ub, (b1 - b0) * ub);
        } else if (io(false, fileOf(t, blk), b0 * ub, buf, (b1 - b0) * ub)) {
            std::memset(buf + size_t(f - b0) * ub, 0, size_t(z1 - f) * ub);
            io(true, fileOf(t, blk), b0 * ub, buf, (b1 - b0) * ub);
        }
        editNext_[t] = z1 - A;
        return true;
    }
    // Peaks: the blocks that changed, from where the audio is now.
    const uint32_t newEnd = cut ? end - (B - A) : end;
    const uint32_t lastBlk = ((cut ? newEnd : B) + bu - 1) / bu;
    const uint32_t blk = editNext_[t];
    if (blk < lastBlk && blk * bu < newEnd) {
        editNext_[t] = blk + 1;
        Ctx p = c;
        p.len = newEnd;
        const uint32_t b0 = blk * bu, n = b0 + bu < newEnd ? bu : newEnd - b0;
        uint8_t* src = c.head && b0 < c.head ? c.ram + size_t(b0) * ub : p_->scratch;
        if (src == c.ram + size_t(b0) * ub || io(false, fileOf(t, blk), b0 * ub, src, n * ub)) peaks(t, p, blk, src);
        return true;
    }
    sv.rStart.store(c.head, std::memory_order_release);   // the ring refills from the files
    sv.rEnd.store(c.head, std::memory_order_release);
    editPhase_[t] = 0;
    sv.editDone.store(true, std::memory_order_release);
    return true;
}

bool Streamer::serviceTrack(int t) {
    StreamView& sv = e_->stream(t);
    if (!sv.active.load(std::memory_order_acquire) || hA_[t] < 0) {
        pend_[t] = false;   // cleared (or never on the card): nothing of it goes back
        undoPhase_[t] = 0;
        editPhase_[t] = 0;
        return false;
    }
    const bool mono = e_->view(t).mono.load(std::memory_order_relaxed);
    Ctx c;
    c.sv = &sv;
    c.ram = reinterpret_cast<uint8_t*>(e_->memory(t).frames);
    c.ub = mono ? 2u : 4u;
    c.bu = kStreamBlockBytes / c.ub;
    c.head = sv.head.load(std::memory_order_acquire);
    c.ring = sv.ring.load(std::memory_order_relaxed);
    const TrackView& v = e_->view(t);
    const uint32_t start = v.start.load(std::memory_order_relaxed);   // a trim: the loop in its buffer
    c.len = sv.recording.load(std::memory_order_acquire) ? sv.recorded.load(std::memory_order_relaxed)
                                                          : start + v.len.load(std::memory_order_relaxed);
    const uint32_t take = sv.take.load(std::memory_order_acquire);
    if (take != take_[t]) {   // a new take: all on file A, no pass yet
        take_[t] = take;
        std::memset(cur_[t], 0, sizeof cur_[t]);
        std::memset(sess_[t], 0, sizeof sess_[t]);
        session_[t] = sv.session.load(std::memory_order_relaxed);
        pend_[t] = false;
        undoPhase_[t] = 0;
        editPhase_[t] = 0;
    }
    if (drain(t, c)) return true;
    if (sv.recording.load(std::memory_order_acquire) || !c.head) return false;

    // A new overdub pass: a new undo set (the pass before can no longer come back).
    const uint32_t ses = sv.session.load(std::memory_order_acquire);
    if (ses != session_[t]) {
        session_[t] = ses;
        std::memset(sess_[t], 0, sizeof sess_[t]);
    }
    if (sv.undoReq.load(std::memory_order_acquire) && !sv.undoDone.load(std::memory_order_relaxed))
        return undoStep(t, c);
    if (sv.editReq.load(std::memory_order_acquire) && !sv.editDone.load(std::memory_order_relaxed))
        return editStep(t, c);

    // Playing: the ring holds [rStart, rEnd), whole blocks around the playhead:
    // reading forwards from kStreamBehind before it (from head while it is in
    // the RAM part), backwards up to kStreamBehind after it (to the loop's end
    // while a reverse loop is in the RAM part: it wraps there next).
    const uint32_t len = c.len, head = c.head, ring = c.ring, bu = c.bu;
    const uint32_t pos = start + v.pos.load(std::memory_order_relaxed);   // buffer frames
    const uint32_t behind = mono ? 2 * kStreamBehind : kStreamBehind;
    const bool back = sv.back.load(std::memory_order_relaxed);
    const bool pp = modeInfo(PlayMode(v.mode.load(std::memory_order_relaxed))).pingPong;
    const bool fwd = pos < head ? !(back && !pp) : !back;   // ping-pong turns at the start: forwards next
    // The loop's start, if it is past head (trimmed into the card part), is
    // where a forward wrap lands.
    const uint32_t first = start > head ? head + (start - head) / bu * bu : head;
    uint32_t s;   // forwards: where the ring starts; backwards: where it ends
    if (fwd) {
        s = pos < first + behind ? first : pos - behind;
        if (s >= len) s = first;
        s = head + (s - head) / bu * bu;
    } else {
        s = pos < head || pos < start + behind || pos + behind >= len ? len : pos + behind;
        if (s < len) s = head + (s - head + bu - 1) / bu * bu;
        if (s > len) s = len;
    }
    uint32_t rS = sv.rStart.load(std::memory_order_relaxed), rE = sv.rEnd.load(std::memory_order_relaxed);
    if (!pend_[t] && (s < rS || s > rE)) {   // a jump or the loop's wrap: start over at s
        sv.rEnd.store(rS, std::memory_order_release);   // empty first: an overdub stops writing it
        pend_[t] = true;
        pendS_[t] = rS;
        pendE_[t] = rE;
    }
    if (pend_[t]) {   // what the overdub changed there goes back before the slots are reused
        if (writeBack(t, c, pos, true, pendS_[t], pendE_[t])) return true;
        pend_[t] = false;
        sv.rStart.store(s, std::memory_order_release);
        sv.rEnd.store(s, std::memory_order_release);
        rS = rE = s;
    }
    // Changed blocks the playhead has left go back to the card.
    if (writeBack(t, c, pos, false, 0, len)) return true;

    if (!fwd) {   // backwards: the block below the ring, the top one makes room
        // Whole blocks up from s - ring, so the ring never holds more than it can.
        const uint32_t lo = s > head + ring ? head + (s - ring - head + bu - 1) / bu * bu : head;
        if (rS <= lo) return false;
        const uint32_t from = head + (rS - 1 - head) / bu * bu;
        if (rE > from + ring) {
            if (writeBack(t, c, pos, true, from + ring, rE)) return true;
            sv.rEnd.store(from + ring, std::memory_order_release);
        }
        if (!io(false, fileOf(t, from / bu), from * c.ub, c.slot(from), (rS - from) * c.ub)) return false;
        sv.rStart.store(from, std::memory_order_release);
        return true;
    }
    const uint32_t want = s + ring < len ? s + ring : len;
    if (rE >= want) return false;
    uint32_t n = want - rE;
    if (n > bu) n = bu;
    if (rE + n > rS + ring) {   // the oldest block makes room: written back first if changed
        const uint32_t nS = rE + n - ring;
        if (writeBack(t, c, pos, true, rS, nS)) return true;
        sv.rStart.store(nS, std::memory_order_release);
    }
    if (!io(false, fileOf(t, rE / bu), rE * c.ub, c.slot(rE), n * c.ub)) return false;
    sv.rEnd.store(rE + n, std::memory_order_release);
    return true;
}

bool Streamer::read(int t, uint32_t b, uint8_t* dst, uint32_t n) {
    StreamView& sv = e_->stream(t);
    const bool mono = e_->view(t).mono.load(std::memory_order_relaxed);
    const uint32_t ub = mono ? 2u : 4u, bu = kStreamBlockBytes / ub;
    const uint32_t head = sv.head.load(std::memory_order_acquire), ring = sv.ring.load(std::memory_order_relaxed);
    const uint8_t* ram = reinterpret_cast<const uint8_t*>(e_->memory(t).frames);
    while (n) {
        uint32_t k = bu - b % bu;   // to the end of this block
        if (k > n) k = n;
        if (!head || b < head) {
            if (head && b + k > head) k = head - b;
            std::memcpy(dst, ram + size_t(b) * ub, size_t(k) * ub);
        } else {
            const uint32_t blk = b / bu;
            const uint32_t rS = sv.rStart.load(std::memory_order_relaxed), rE = sv.rEnd.load(std::memory_order_relaxed);
            const bool changed = (sv.dirty[blk >> 5].load(std::memory_order_relaxed) >> (blk & 31)) & 1u;
            if (changed && b >= rS && b + k <= rE) {   // newer in the ring than on the card
                std::memcpy(dst, ram + size_t(head + (b - head) % ring) * ub, size_t(k) * ub);
            } else if (hA_[t] < 0 || !io(false, fileOf(t, blk), b * ub, dst, k * ub)) {
                return false;
            }
        }
        dst += size_t(k) * ub;
        b += k;
        n -= k;
    }
    return true;
}

bool Streamer::write(int t, bool mono, uint32_t b, const uint8_t* src, uint32_t n) {
    const uint32_t ub = mono ? 2u : 4u;
    return hA_[t] >= 0 && !e_->stream(t).active.load(std::memory_order_acquire) &&
           io(true, hA_[t], b * ub, const_cast<uint8_t*>(src), n * ub);
}

void Streamer::service() {
    if (!e_ || !p_) return;
    if (!cardOk()) {
        if (hA_[0] >= 0 || hA_[1] >= 0 || hA_[2] >= 0 || hA_[3] >= 0 || tried_[0]) closeAll();
        return;
    }
    setUp();
    // One command per track at most, the tracks in turn.
    for (int i = 0; i < kTracks; ++i) serviceTrack((next_ + i) % kTracks);
    next_ = (next_ + 1) % kTracks;
}

}  // namespace looper
