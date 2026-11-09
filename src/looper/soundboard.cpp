#include "looper/soundboard.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "looper/engine.h"
#include "looper/files.h"

namespace looper {

namespace {

using ot::IStorage;

constexpr const char* kBoardFile = "Looper/soundboard.txt";
constexpr const char* kBoardTmp = "Looper/soundboard.tmp";
constexpr float kFadeStep = 1.0f / 128.0f;   // a stop fades out over 128 frames (~3 ms)

// The file name without its folders and ".wav".
void baseName(const char* path, char* out, size_t n) {
    const char* b = std::strrchr(path, '/');
    b = b ? b + 1 : path;
    size_t len = std::strlen(b);
    if (len > 4 && b[len - 4] == '.' && (b[len - 3] | 32) == 'w' && (b[len - 2] | 32) == 'a' && (b[len - 1] | 32) == 'v')
        len -= 4;
    if (len >= n) len = n - 1;
    std::memcpy(out, b, len);
    out[len] = 0;
}

}  // namespace

void Soundboard::init(const Platform* p, uint8_t* mem) {
    p_ = p;
    mem_ = mem;
    slots_ = reinterpret_cast<Slot*>(mem + size_t(kSlots) * kHeadBytes + size_t(kVoices) * kRingBytes);
    std::memset(slots_, 0, kSlotBytes);
}

bool Soundboard::cardOk() const { return p_ && p_->storage && (!p_->storageReady || p_->storageReady()); }

bool Soundboard::sounding(const Voice& v) const {
    return v.gen.load(std::memory_order_relaxed) != v.ackGen.load(std::memory_order_acquire) ||
           v.active.load(std::memory_order_acquire);
}

void Soundboard::markDirty() {
    dirty_ = true;
    dirtyMs_ = p_ && p_->clock ? p_->clock->millis() : 0;
}

void Soundboard::service() {
    if (!cardOk()) {
        if (wasReady_) {   // the card went: what streams from it stops
            for (Voice& v : voices_) {
                if (v.h >= 0 || v.left) v.stopGen.store(v.gen.load(std::memory_order_relaxed), std::memory_order_release);
                closeVoice(v);
            }
        }
        wasReady_ = false;
        return;
    }
    if (!wasReady_) {
        wasReady_ = true;
        boardLoaded_ = false;
    }
    if (!boardLoaded_) {
        boardLoaded_ = true;
        loadBoard();
        return;
    }
    for (Voice& v : voices_)   // a play that ended: its file closes
        if (v.h >= 0 && !sounding(v)) closeVoice(v);
    // Feed the voices, the one with the least read ahead first: two card reads at most.
    for (int k = 0; k < 2; ++k) {
        Voice* best = nullptr;
        uint32_t bestAhead = 0xFFFFFFFFu;
        for (Voice& v : voices_) {
            if (v.h < 0 || !v.left) continue;
            const Slot& s = slots_[v.slot];
            const uint32_t pos = v.ackGen.load(std::memory_order_acquire) == v.gen.load(std::memory_order_relaxed)
                                     ? v.pos.load(std::memory_order_relaxed) : 0;
            const uint32_t used = pos > s.headFrames ? pos - s.headFrames : 0;
            const uint32_t ahead = v.written > used ? v.written - used : 0;
            if (ahead < bestAhead) {
                bestAhead = ahead;
                best = &v;
            }
        }
        if (!best || !feed(*best)) break;
    }
    // One slot start a call.
    for (int i = 0; i < kSlots; ++i)
        if (slots_[i].state == SlotState::kPending) {
            loadHead(i);
            break;
        }
    if (dirty_ && p_->clock && int32_t(p_->clock->millis() - dirtyMs_) >= 1000) {
        dirty_ = false;
        saveBoard();
    }
}

bool Soundboard::feed(Voice& v) {
    const Slot& s = slots_[v.slot];
    const uint32_t fb = s.mono ? 2 : 4;
    const uint32_t ringFrames = kRingBytes / fb, chunk = kChunkBytes / fb;
    const uint32_t pos = v.ackGen.load(std::memory_order_acquire) == v.gen.load(std::memory_order_relaxed)
                             ? v.pos.load(std::memory_order_relaxed) : 0;
    const uint32_t used = pos > s.headFrames ? pos - s.headFrames : 0;
    if (v.written + chunk - used > ringFrames) return false;   // the ring is full
    const uint32_t n = v.left < chunk ? v.left : chunk;
    uint8_t* dst = ringOf(int(&v - voices_)) + size_t(v.written % ringFrames) * fb;
    if (p_->storage->read(v.h, dst, n * fb) != int32_t(n * fb)) {
        std::snprintf(status_, sizeof status_, "Reading %.30s failed", s.name);
        v.stopGen.store(v.gen.load(std::memory_order_relaxed), std::memory_order_release);
        closeVoice(v);
        return true;
    }
    v.written += n;
    v.left -= n;
    v.avail.store(v.written, std::memory_order_release);
    if (!v.left) {
        p_->storage->close(v.h);
        v.h = -1;
    }
    return true;
}

void Soundboard::closeVoice(Voice& v) {
    if (v.h >= 0) p_->storage->close(v.h);
    v.h = -1;
    v.left = 0;
}

bool Soundboard::loadHead(int i) {
    Slot& s = slots_[i];
    IStorage& fs = *p_->storage;
    const IStorage::Handle h = fs.open(s.path, IStorage::kRead);
    uint32_t ch = 0, rate = 0, bytes = 0, off = 0;
    const char* why = nullptr;
    if (h < 0) why = "not on the card";
    else if (!wavInfo(fs, h, ch, rate, bytes, off) || (ch != 1 && ch != 2)) why = "not 16-bit PCM";
    else if (rate != kSampleRate) why = "not 44.1 kHz";
    else if (bytes < ch * 2) why = "empty";
    if (!why) {
        const uint32_t fb = ch * 2;
        s.mono = ch == 1;
        s.frames = bytes / fb;
        s.dataOff = off;
        s.headFrames = s.frames < kHeadBytes / fb ? s.frames : kHeadBytes / fb;
        if (fs.read(h, headOf(i), s.headFrames * fb) != int32_t(s.headFrames * fb)) why = "a card error";
    }
    if (h >= 0) fs.close(h);
    if (why) {
        s.state = SlotState::kMissing;
        std::snprintf(status_, sizeof status_, "Pad %d: %.24s is %s", i + 1, s.name, why);
        return false;
    }
    s.state = SlotState::kReady;
    return true;
}

bool Soundboard::assign(int i, const char* path) {
    if (i < 0 || i >= kSlots || !path || !path[0]) return false;
    if (std::strlen(path) >= size_t(kPathLen)) {
        std::snprintf(status_, sizeof status_, "The path is too long for a pad");
        return false;
    }
    if (!cardOk()) {
        std::snprintf(status_, sizeof status_, "No SD card");
        return false;
    }
    stop(i);
    for (Voice& v : voices_)
        if (v.slot == i) closeVoice(v);
    Slot& s = slots_[i];
    std::snprintf(s.path, sizeof s.path, "%s", path);
    baseName(path, s.name, sizeof s.name);
    s.state = SlotState::kPending;
    markDirty();
    if (!loadHead(i)) return false;
    std::snprintf(status_, sizeof status_, "Pad %d: %.40s", i + 1, s.name);
    return true;
}

void Soundboard::clear(int i) {
    if (i < 0 || i >= kSlots) return;
    stop(i);
    for (Voice& v : voices_)
        if (v.slot == i) closeVoice(v);
    std::memset(&slots_[i], 0, sizeof(Slot));
    std::snprintf(status_, sizeof status_, "Pad %d cleared", i + 1);
    markDirty();
}

void Soundboard::start(int i) {
    const Slot& s = slots_[i];
    // A voice that is free; else the one started first (not one whose start
    // the audio thread has not taken yet).
    Voice* v = nullptr;
    for (Voice& c : voices_)
        if (!sounding(c)) {
            v = &c;
            break;
        }
    if (!v)
        for (Voice& c : voices_)
            if (c.gen.load(std::memory_order_relaxed) == c.ackGen.load(std::memory_order_acquire) &&
                (!v || c.order < v->order))
                v = &c;
    if (!v) return;
    closeVoice(*v);
    const uint32_t fb = s.mono ? 2 : 4;
    uint32_t frames = s.frames;
    v->left = s.frames - s.headFrames;
    if (v->left) {   // the rest from the card, after the part in SDRAM
        v->h = p_->storage->open(s.path, IStorage::kRead);
        if (v->h < 0 || !p_->storage->seek(v->h, s.dataOff + s.headFrames * fb)) {
            closeVoice(*v);
            frames = s.headFrames;   // the start alone
            std::snprintf(status_, sizeof status_, "Pad %d: the card did not open it", i + 1);
        }
    }
    v->next = Play_{reinterpret_cast<const int16_t*>(headOf(i)), reinterpret_cast<const int16_t*>(ringOf(int(v - voices_))),
                    s.headFrames, frames, kRingBytes / fb, s.mono, uint8_t(i)};
    v->written = 0;
    v->slot = i;
    v->order = ++order_;
    v->avail.store(0, std::memory_order_relaxed);
    v->gen.store(v->gen.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

void Soundboard::press(int i) {
    if (i < 0 || i >= kSlots) return;
    const Slot& s = slots_[i];
    if (s.state != SlotState::kReady) {
        if (s.state == SlotState::kEmpty) std::snprintf(status_, sizeof status_, "Pad %d is empty: F1 loads a WAV", i + 1);
        return;
    }
    if (mode_ == Play::kToggle && progress(i) >= 0.0f) {
        stop(i);
        return;
    }
    stopAll();   // one at a time: what plays fades out (one-shot and gate start it again from the top)
    start(i);
}

void Soundboard::release(int i) {
    if (mode_ == Play::kGate) stop(i);
}

void Soundboard::stop(int i) {
    for (Voice& v : voices_)
        if (v.slot == i && sounding(v)) v.stopGen.store(v.gen.load(std::memory_order_relaxed), std::memory_order_release);
}

void Soundboard::stopAll() {
    for (Voice& v : voices_)
        if (sounding(v)) v.stopGen.store(v.gen.load(std::memory_order_relaxed), std::memory_order_release);
}

float Soundboard::progress(int i) const {
    float best = -1.0f;
    uint32_t order = 0;
    for (const Voice& v : voices_) {
        if (v.slot != i || !sounding(v)) continue;
        const uint32_t g = v.gen.load(std::memory_order_relaxed);
        if (v.stopGen.load(std::memory_order_relaxed) == g || v.order < order) continue;   // stopping
        order = v.order;
        const uint32_t pos = v.ackGen.load(std::memory_order_acquire) == g ? v.pos.load(std::memory_order_relaxed) : 0;
        const uint32_t frames = slots_[i].frames ? slots_[i].frames : 1;
        best = float(pos) / float(frames);
    }
    return best > 1.0f ? 1.0f : best;
}

int Soundboard::playing() const {
    int n = 0;
    for (const Voice& v : voices_) n += sounding(v) ? 1 : 0;
    return n;
}

bool Soundboard::loading() const {
    for (int i = 0; i < kSlots; ++i)
        if (slots_[i].state == SlotState::kPending) return true;
    return false;
}

void Soundboard::setMode(Play m) {
    mode_ = m;
    markDirty();
}

void Soundboard::setLevel(int v) {
    level_.store(v < 0 ? 0 : (v > 100 ? 100 : v), std::memory_order_relaxed);
    markDirty();
}

void Soundboard::setToLoops(bool on) {
    toLoops_ = on;
    markDirty();
}

bool Soundboard::render(float* l, float* r, uint32_t n) {
    const float g = float(level_.load(std::memory_order_relaxed)) / 100.0f;
    bool any = false;
    for (Voice& v : voices_) {
        const uint32_t gen = v.gen.load(std::memory_order_acquire);
        if (gen != v.ackGen.load(std::memory_order_relaxed)) {   // a new play
            v.cur = v.next;
            v.pos.store(0, std::memory_order_relaxed);
            v.fade = 1.0f;
            v.active.store(true, std::memory_order_relaxed);
            v.ackGen.store(gen, std::memory_order_release);
        }
        if (!v.active.load(std::memory_order_relaxed)) continue;
        const bool stopping = v.stopGen.load(std::memory_order_acquire) == gen;
        const Play_& c = v.cur;
        const uint32_t avail = v.avail.load(std::memory_order_acquire);
        const uint32_t step = c.mono ? 1 : 2;
        uint32_t p = v.pos.load(std::memory_order_relaxed);
        for (uint32_t f = 0; f < n; ++f) {
            if (p >= c.frames) {
                v.active.store(false, std::memory_order_release);
                break;
            }
            const int16_t* s;
            if (p < c.headFrames) {
                s = c.head + size_t(p) * step;
            } else {
                const uint32_t q = p - c.headFrames;
                if (q >= avail) {   // the card is behind: silence until it catches up (a stop ends here)
                    underruns_.fetch_add(1, std::memory_order_relaxed);
                    if (stopping) v.active.store(false, std::memory_order_release);
                    break;
                }
                s = c.ring + size_t(q % c.ringFrames) * step;
            }
            float k = g;
            if (stopping) {
                v.fade -= kFadeStep;
                if (v.fade <= 0.0f) {
                    v.active.store(false, std::memory_order_release);
                    break;
                }
                k *= v.fade;
            }
            l[f] += float(s[0]) * k;
            r[f] += float(s[c.mono ? 0 : 1]) * k;
            any = true;
            ++p;
        }
        v.pos.store(p, std::memory_order_release);
    }
    return any;
}

// ---------------------------------------------------------------------------
// The board file: "mode N", "level N", "loops N", then "pad N path" lines.

void Soundboard::loadBoard() {
    IStorage& fs = *p_->storage;
    const IStorage::Handle h = fs.open(kBoardFile, IStorage::kRead);
    if (h < 0) return;
    char buf[256], line[kPathLen + 16];
    int len = 0;
    auto take = [&](char* ln) {
        int n = 0;
        if (std::sscanf(ln, "mode %d", &n) == 1) mode_ = Play(n >= 0 && n <= 2 ? n : 0);
        else if (std::sscanf(ln, "level %d", &n) == 1) level_.store(n < 0 ? 0 : (n > 100 ? 100 : n));
        else if (std::sscanf(ln, "loops %d", &n) == 1) toLoops_ = n != 0;
        else if (!std::strncmp(ln, "pad ", 4)) {
            char* sp = std::strchr(ln + 4, ' ');
            n = std::atoi(ln + 4);
            if (!sp || n < 1 || n > kSlots || !sp[1]) return;
            Slot& s = slots_[n - 1];
            std::snprintf(s.path, sizeof s.path, "%s", sp + 1);
            baseName(s.path, s.name, sizeof s.name);
            s.state = SlotState::kPending;
        }
    };
    for (;;) {
        const int32_t got = fs.read(h, buf, sizeof buf);
        if (got <= 0) break;
        for (int32_t i = 0; i < got; ++i) {
            const char c = buf[i];
            if (c == '\n' || c == '\r') {
                line[len] = 0;
                if (len) take(line);
                len = 0;
            } else if (len < int(sizeof line) - 1) {
                line[len++] = c;
            }
        }
    }
    line[len] = 0;
    if (len) take(line);
    fs.close(h);
}

void Soundboard::saveBoard() {
    if (!cardOk()) {
        dirty_ = true;   // tried again later
        dirtyMs_ = p_->clock ? p_->clock->millis() + 9000 : 0;
        return;
    }
    IStorage& fs = *p_->storage;
    fs.mkdir("Looper");
    const IStorage::Handle h = fs.open(kBoardTmp, IStorage::kWrite);
    if (h < 0) return;
    char line[kPathLen + 16];
    bool ok = true;
    auto put = [&](int n) { ok = ok && n > 0 && fs.write(h, line, uint32_t(n)) == n; };
    put(std::snprintf(line, sizeof line, "mode %d\nlevel %d\nloops %d\n", int(mode_), level(), toLoops_ ? 1 : 0));
    for (int i = 0; i < kSlots; ++i)
        if (slots_[i].state != SlotState::kEmpty) put(std::snprintf(line, sizeof line, "pad %d %s\n", i + 1, slots_[i].path));
    fs.close(h);
    if (!ok) {
        fs.remove(kBoardTmp);
        return;
    }
    if (fs.exists(kBoardFile)) fs.remove(kBoardFile);
    fs.rename(kBoardTmp, kBoardFile);
}

}  // namespace looper
