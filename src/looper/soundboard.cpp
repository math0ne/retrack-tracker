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

// One sample as the file holds it, in int16 scale.
inline float sampleOf(const uint8_t* q, uint8_t bytes, uint8_t isFloat) {
    switch (bytes) {
        case 1: return float(int(q[0]) - 128) * 256.0f;
        case 2: return float(int16_t(uint16_t(q[0] | q[1] << 8)));
        case 3: return float(int32_t(uint32_t(q[0]) << 8 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 24)) * (1.0f / 65536.0f);
        default: {
            const uint32_t u = uint32_t(q[0]) | uint32_t(q[1]) << 8 | uint32_t(q[2]) << 16 | uint32_t(q[3]) << 24;
            if (!isFloat) return float(int32_t(u)) * (1.0f / 65536.0f);
            float x;
            std::memcpy(&x, &u, 4);
            return x > -8.0f && x < 8.0f ? x * 32767.0f : 0.0f;   // NaN and wild values: silence
        }
    }
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
            if (scanH_ >= 0) {
                p_->storage->close(scanH_);
                scanH_ = -1;
                scanFailed_ = true;
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
            const uint32_t pos = v.ackGen.load(std::memory_order_acquire) == v.gen.load(std::memory_order_relaxed)
                                     ? v.pos.load(std::memory_order_relaxed) : 0;
            const uint32_t used = pos > v.next.headFrames ? pos - v.next.headFrames : 0;
            const uint32_t ahead = v.written > used ? v.written - used : 0;
            if (ahead < bestAhead) {
                bestAhead = ahead;
                best = &v;
            }
        }
        if (!best || !feed(*best)) break;
    }
    // One slot start a call; a moved start once it has stayed put and the pad is quiet.
    const uint32_t now = p_->clock ? p_->clock->millis() : 0;
    for (int i = 0; i < kSlots; ++i) {
        Slot& s = slots_[i];
        if (s.state == SlotState::kPending) {
            openSlot(i, true);
            break;
        }
        if (s.state == SlotState::kReady && s.headStale && int32_t(now - partMs_) >= 300 && !slotSounding(i)) {
            const IStorage::Handle h = p_->storage->open(s.path, IStorage::kRead);
            if (h < 0 || !readHead(i, h)) {   // a press streams it all from the card meanwhile
                s.headStale = 1;
                partMs_ = now;
            }
            if (h >= 0) p_->storage->close(h);
            break;
        }
    }
    if (scanH_ >= 0) scanStep();   // the pad editor's waveform: a read a call
    if (dirty_ && p_->clock && int32_t(p_->clock->millis() - dirtyMs_) >= 1000) {
        dirty_ = false;
        saveBoard();
    }
}

bool Soundboard::feed(Voice& v) {
    const Slot& s = slots_[v.slot];
    const uint32_t fb = s.frameBytes;
    const uint32_t ringF = ringFrames(fb), chunk = chunkFrames(fb);
    const uint32_t pos = v.ackGen.load(std::memory_order_acquire) == v.gen.load(std::memory_order_relaxed)
                             ? v.pos.load(std::memory_order_relaxed) : 0;
    const uint32_t used = pos > v.next.headFrames ? pos - v.next.headFrames : 0;
    if (v.written + chunk - used > ringF) return false;   // the ring is full
    const uint32_t n = v.left < chunk ? v.left : chunk;
    uint8_t* dst = ringOf(int(&v - voices_)) + size_t(v.written % ringF) * fb;
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

const char* Soundboard::whyText(uint8_t why) {
    switch (why) {
        case kNoFile: return "not on the card";
        case kNotWav: return "not a WAV";
        case kCompressed: return "compressed (not PCM)";
        case kBits: return "an odd bit depth";
        case kChannels: return "odd channels";
        case kRate: return "an odd sample rate";
        case kNoSound: return "empty";
        case kCardError: return "a card error";
        default: return "";
    }
}

bool Soundboard::openSlot(int i, bool head) {
    Slot& s = slots_[i];
    IStorage& fs = *p_->storage;
    const IStorage::Handle h = fs.open(s.path, IStorage::kRead);
    WavFormat f;
    uint8_t why = kOk;
    if (h < 0) {
        why = kNoFile;
    } else {
        switch (wavFormat(fs, h, f)) {
            case WavError::kNone: break;
            case WavError::kCompressed: why = kCompressed; break;
            case WavError::kBits: why = kBits; break;
            case WavError::kChannels: why = kChannels; break;
            case WavError::kRate: why = kRate; break;
            default: why = kNotWav; break;
        }
    }
    s.headFrames = 0;
    if (!why) {
        s.mono = f.channels == 1;
        s.sampleBytes = uint8_t(f.sampleBytes);
        s.frameBytes = uint8_t(f.frameBytes);
        s.isFloat = f.isFloat;
        s.rate = f.rate;
        s.dataOff = f.dataOff;
        s.frames = f.dataBytes / f.frameBytes;
        if (!s.frames) why = kNoSound;
    }
    if (!why) {   // the part: the whole file unless the board says less
        if (!s.to || s.to > s.frames) s.to = s.frames;
        if (s.from >= s.to) s.from = 0;
    }
    if (!why && head && !readHead(i, h)) why = kCardError;
    if (h >= 0) fs.close(h);
    if (why) {
        s.state = SlotState::kMissing;
        s.why = why;
        if (i == kPreview) std::snprintf(status_, sizeof status_, "%.30s: %s", s.name, whyText(why));
        else std::snprintf(status_, sizeof status_, "Pad %d: %.20s is %s", i + 1, s.name, whyText(why));
        return false;
    }
    s.state = SlotState::kReady;
    s.why = kOk;
    return true;
}

bool Soundboard::readHead(int i, IStorage::Handle h) {
    Slot& s = slots_[i];
    const uint32_t fb = s.frameBytes, part = s.to - s.from;
    s.headFrames = 0;
    s.headStale = 0;
    if (!p_->storage->seek(h, s.dataOff + s.from * fb)) return false;
    const uint32_t n = part < kHeadBytes / fb ? part : kHeadBytes / fb;
    if (p_->storage->read(h, headOf(i), n * fb) != int32_t(n * fb)) return false;
    s.headFrames = n;
    return true;
}

uint32_t Soundboard::outFrames(int i, uint32_t f) const {
    const Slot& s = slots_[i];
    return s.rate ? uint32_t(uint64_t(f) * kSampleRate / s.rate) : 0;
}

uint32_t Soundboard::length(int i) const { return outFrames(i, slots_[i].to - slots_[i].from); }

bool Soundboard::slotSounding(int i) const {
    for (const Voice& v : voices_)
        if (v.slot == i && sounding(v)) return true;
    return false;
}

void Soundboard::setPart(int i, uint32_t from, uint32_t to) {
    if (i < 0 || i >= kSlots) return;
    Slot& s = slots_[i];
    if (s.state != SlotState::kReady) return;
    const uint32_t minF = s.rate / 1000 ? s.rate / 1000 : 1;
    if (s.frames <= minF) return;
    if (to > s.frames) to = s.frames;
    if (to < minF) to = minF;
    if (from + minF > to) from = to - minF;
    if (from != s.from) {   // the SDRAM part starts elsewhere: reloaded once the start stays put
        s.headStale = 1;
        partMs_ = p_->clock ? p_->clock->millis() : 0;
    }
    if (from == s.from && to == s.to) return;
    s.from = from;
    s.to = to;
    markDirty();
}

void Soundboard::scan(int i) {
    if (i == scanSlot_ && !scanFailed_) return;   // read already, or being read
    if (scanH_ >= 0) p_->storage->close(scanH_);
    scanH_ = -1;
    scanSlot_ = -1;
    if (i < 0 || i >= kSlots || slots_[i].state != SlotState::kReady || !cardOk()) return;
    std::memset(mem_ + kPeakOff, 0, kPeakBytes);
    scanSlot_ = i;
    scanDone_ = 0;
    scanFailed_ = !scanBuf_;
    ++scanVer_;
    if (scanFailed_) return;
    const Slot& s = slots_[i];
    scanH_ = p_->storage->open(s.path, IStorage::kRead);
    if (scanH_ < 0 || !p_->storage->seek(scanH_, s.dataOff)) {
        if (scanH_ >= 0) p_->storage->close(scanH_);
        scanH_ = -1;
        scanFailed_ = true;
    }
}

void Soundboard::scanStep() {
    // One read of the file: the loudest sample of each column it touches.
    const Slot& s = slots_[scanSlot_];
    const uint32_t fb = s.frameBytes;
    const uint32_t cap = (scanBufBytes_ < kChunkBytes ? scanBufBytes_ : kChunkBytes) / fb;
    const uint32_t n = s.frames - scanDone_ < cap ? s.frames - scanDone_ : cap;
    if (!n || p_->storage->read(scanH_, scanBuf_, n * fb) != int32_t(n * fb)) {
        p_->storage->close(scanH_);
        scanH_ = -1;
        scanFailed_ = n != 0;
        ++scanVer_;
        return;
    }
    uint8_t* pk[2] = {mem_ + kPeakOff, mem_ + kPeakOff + kPeakCols};
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t col = uint32_t(uint64_t(scanDone_ + k) * kPeakCols / s.frames);
        const uint8_t* q = scanBuf_ + size_t(k) * fb;
        for (int ch = 0; ch < 2; ++ch) {
            float x = sampleOf(q + (s.mono ? 0 : ch * s.sampleBytes), s.sampleBytes, s.isFloat);
            if (x < 0.0f) x = -x;
            const int a = x >= 32767.0f ? 255 : int(x * (255.0f / 32767.0f));
            if (a > pk[ch][col]) pk[ch][col] = uint8_t(a);
        }
    }
    scanDone_ += n;
    ++scanVer_;
    if (scanDone_ >= s.frames) {
        p_->storage->close(scanH_);
        scanH_ = -1;
    }
}

float Soundboard::scanProgress() const {
    if (scanSlot_ < 0 || scanFailed_) return -1.0f;
    const uint32_t f = slots_[scanSlot_].frames;
    return f ? float(scanDone_) / float(f) : 1.0f;
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
    if (scanSlot_ == i) scan(-1);
    Slot& s = slots_[i];
    std::snprintf(s.path, sizeof s.path, "%s", path);
    baseName(path, s.name, sizeof s.name);
    s.from = s.to = 0;   // a new WAV: all of it
    s.state = SlotState::kPending;
    markDirty();
    if (!openSlot(i, true)) return false;
    std::snprintf(status_, sizeof status_, "Pad %d: %.40s", i + 1, s.name);
    return true;
}

void Soundboard::clear(int i) {
    if (i < 0 || i >= kSlots) return;
    stop(i);
    for (Voice& v : voices_)
        if (v.slot == i) closeVoice(v);
    if (scanSlot_ == i) scan(-1);
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
    const uint32_t fb = s.frameBytes;
    uint8_t* ring = ringOf(int(v - voices_));
    // The part: its start from SDRAM (none while that is being moved), the rest from the card.
    uint32_t frames = s.to - s.from;
    const uint32_t head = s.headStale ? 0 : (s.headFrames < frames ? s.headFrames : frames);
    v->written = 0;
    v->left = frames - head;
    if (v->left) {   // the rest from the card, after the part in SDRAM
        v->h = p_->storage->open(s.path, IStorage::kRead);
        if (v->h < 0 || !p_->storage->seek(v->h, s.dataOff + (s.from + head) * fb)) {
            closeVoice(*v);
            frames = head;   // the start alone
            std::snprintf(status_, sizeof status_, "%.30s: the card did not open it", s.name);
        } else if (!head) {   // nothing in SDRAM (the preview, a moved start): its first chunk now
            const uint32_t n = v->left < chunkFrames(fb) ? v->left : chunkFrames(fb);
            if (p_->storage->read(v->h, ring, n * fb) == int32_t(n * fb)) {
                v->written = n;
                v->left -= n;
            }
        }
    }
    v->next = Play_{head ? headOf(i) : nullptr, ring, head, frames, ringFrames(fb),
                    uint32_t((uint64_t(s.rate) << 16) / kSampleRate), s.mono, uint8_t(i), s.sampleBytes, s.frameBytes,
                    s.isFloat};
    v->slot = i;
    v->order = ++order_;
    v->avail.store(v->written, std::memory_order_relaxed);
    v->gen.store(v->gen.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

void Soundboard::press(int i) {
    if (i < 0 || i >= kSlots) return;
    const Slot& s = slots_[i];
    if (s.state != SlotState::kReady) {
        if (s.state == SlotState::kEmpty) std::snprintf(status_, sizeof status_, "Pad %d is empty: F1 loads a WAV", i + 1);
        else if (s.state == SlotState::kMissing)
            std::snprintf(status_, sizeof status_, "Pad %d: %.20s is %s", i + 1, s.name, whyText(s.why));
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

bool Soundboard::preview(const char* path) {
    stopPreview();
    if (!path || !path[0]) return false;
    if (!cardOk()) {
        std::snprintf(status_, sizeof status_, "No SD card");
        return false;
    }
    if (std::strlen(path) >= size_t(kPathLen)) {
        std::snprintf(status_, sizeof status_, "The path is too long to preview");
        return false;
    }
    for (Voice& v : voices_)
        if (v.slot == kPreview) closeVoice(v);
    Slot& s = slots_[kPreview];
    std::memset(&s, 0, sizeof s);
    std::snprintf(s.path, sizeof s.path, "%s", path);
    baseName(path, s.name, sizeof s.name);
    if (!openSlot(kPreview, false)) return false;
    start(kPreview);
    std::snprintf(status_, sizeof status_, "Preview: %.40s", s.name);
    return true;
}

void Soundboard::stopPreview() { stop(kPreview); }

bool Soundboard::previewing() const { return progress(kPreview) >= 0.0f; }

float Soundboard::progress(int i) const {
    float best = -1.0f;
    uint32_t order = 0;
    for (const Voice& v : voices_) {
        if (v.slot != i || !sounding(v)) continue;
        const uint32_t g = v.gen.load(std::memory_order_relaxed);
        if (v.stopGen.load(std::memory_order_relaxed) == g || v.order < order) continue;   // stopping
        order = v.order;
        const uint32_t pos = v.ackGen.load(std::memory_order_acquire) == g ? v.pos.load(std::memory_order_relaxed) : 0;
        const uint32_t frames = v.next.frames ? v.next.frames : 1;
        best = float(pos) / float(frames);
    }
    return best > 1.0f ? 1.0f : best;
}

bool Soundboard::position(int i, uint32_t& frame) const {
    const float p = progress(i);
    if (p < 0.0f) return false;
    const Slot& s = slots_[i];
    frame = s.from + uint32_t(p * float(s.to - s.from));
    return true;
}

int Soundboard::playing() const {
    int n = 0;
    for (const Voice& v : voices_) n += sounding(v) && v.slot != kPreview ? 1 : 0;
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

void Soundboard::setSend(int s, int v) {
    if (s < 0 || s > 1) return;
    send_[s] = uint8_t(v < 0 ? 0 : (v > 100 ? 100 : v));
    markDirty();
}

void Soundboard::setFilter(int v) {
    filter_ = uint16_t(v < 0 ? 0 : (v > int(kFilterFineMax) ? int(kFilterFineMax) : v));
    markDirty();
}

bool Soundboard::render(float* l, float* r, float* ol, float* orr, uint32_t n) {
    const float g = float(level_.load(std::memory_order_relaxed)) / 100.0f;
    bool any = false;
    for (Voice& v : voices_) {
        const uint32_t gen = v.gen.load(std::memory_order_acquire);
        if (gen != v.ackGen.load(std::memory_order_relaxed)) {   // a new play
            v.cur = v.next;
            v.pos.store(0, std::memory_order_relaxed);
            v.frac = 0;
            v.fade = 1.0f;
            v.active.store(true, std::memory_order_relaxed);
            v.ackGen.store(gen, std::memory_order_release);
        }
        if (!v.active.load(std::memory_order_relaxed)) continue;
        const bool stopping = v.stopGen.load(std::memory_order_acquire) == gen;
        const Play_& c = v.cur;
        float* dl = c.slot == kPreview ? ol : l;
        float* dr = c.slot == kPreview ? orr : r;
        const uint32_t avail = v.avail.load(std::memory_order_acquire);
        // File frame i as left / right; false: the card has not brought it yet.
        auto frameAt = [&](uint32_t i, float& a, float& b) {
            const uint8_t* s;
            if (i < c.headFrames) {
                s = c.head + size_t(i) * c.frameBytes;
            } else {
                const uint32_t q = i - c.headFrames;
                if (q >= avail) return false;
                s = c.ring + size_t(q % c.ringFrames) * c.frameBytes;
            }
            a = sampleOf(s, c.sampleBytes, c.isFloat);
            b = c.mono ? a : sampleOf(s + c.sampleBytes, c.sampleBytes, c.isFloat);
            return true;
        };
        uint32_t p = v.pos.load(std::memory_order_relaxed);
        for (uint32_t f = 0; f < n; ++f) {
            if (p >= c.frames) {
                v.active.store(false, std::memory_order_release);
                break;
            }
            float a, b;
            if (!frameAt(p, a, b)) {   // the card is behind: silence until it catches up (a stop ends here)
                underruns_.fetch_add(1, std::memory_order_relaxed);
                if (stopping) v.active.store(false, std::memory_order_release);
                break;
            }
            float a1, b1;   // another rate: between this frame and the next
            if (v.frac && p + 1 < c.frames && frameAt(p + 1, a1, b1)) {
                const float t = float(v.frac) * (1.0f / 65536.0f);
                a += (a1 - a) * t;
                b += (b1 - b) * t;
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
            dl[f] += a * k;
            dr[f] += b * k;
            any = true;
            v.frac += c.step16;
            p += v.frac >> 16;
            v.frac &= 0xFFFFu;
        }
        v.pos.store(p, std::memory_order_release);
    }
    return any;
}

// ---------------------------------------------------------------------------
// The board file: "mode N", "level N", "loops N", "send1 N", "send2 N",
// "filter N", then "pad N path" lines, each with a "part N from to" line
// after it (file frames) when the pad plays less than all of it.

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
        else if (std::sscanf(ln, "send1 %d", &n) == 1) send_[0] = uint8_t(n < 0 ? 0 : (n > 100 ? 100 : n));
        else if (std::sscanf(ln, "send2 %d", &n) == 1) send_[1] = uint8_t(n < 0 ? 0 : (n > 100 ? 100 : n));
        else if (std::sscanf(ln, "filter %d", &n) == 1)
            filter_ = uint16_t(n < 0 ? 0 : (n > int(kFilterFineMax) ? int(kFilterFineMax) : n));
        else if (!std::strncmp(ln, "pad ", 4)) {
            char* sp = std::strchr(ln + 4, ' ');
            n = std::atoi(ln + 4);
            if (!sp || n < 1 || n > kSlots || !sp[1]) return;
            Slot& s = slots_[n - 1];
            std::snprintf(s.path, sizeof s.path, "%s", sp + 1);
            baseName(s.path, s.name, sizeof s.name);
            s.state = SlotState::kPending;
        } else if (!std::strncmp(ln, "part ", 5)) {   // clamped to the file when it opens
            unsigned long from = 0, to = 0;
            if (std::sscanf(ln + 5, "%d %lu %lu", &n, &from, &to) != 3 || n < 1 || n > kSlots) return;
            Slot& s = slots_[n - 1];
            s.from = uint32_t(from);
            s.to = uint32_t(to);
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
    put(std::snprintf(line, sizeof line, "send1 %d\nsend2 %d\nfilter %d\n", send_[0], send_[1], filter_));
    for (int i = 0; i < kSlots; ++i) {
        const Slot& s = slots_[i];
        if (s.state == SlotState::kEmpty) continue;
        put(std::snprintf(line, sizeof line, "pad %d %s\n", i + 1, s.path));
        if (s.from || (s.to && (!s.frames || s.to < s.frames)))   // (not opened yet: kept as read)
            put(std::snprintf(line, sizeof line, "part %d %lu %lu\n", i + 1, static_cast<unsigned long>(s.from),
                              static_cast<unsigned long>(s.to)));
    }
    fs.close(h);
    if (!ok) {
        fs.remove(kBoardTmp);
        return;
    }
    if (fs.exists(kBoardFile)) fs.remove(kBoardFile);
    fs.rename(kBoardTmp, kBoardFile);
}

}  // namespace looper
