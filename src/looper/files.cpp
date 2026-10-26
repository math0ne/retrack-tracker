#include "looper/files.h"

#include <cstdio>
#include <cstring>

namespace looper {

namespace {

using ot::IStorage;

constexpr uint32_t kChunk = 4096;   // frames per step: 16 KB stereo
constexpr char kRoot[] = "Looper";
constexpr char kProjects[] = "Looper/Projects";
constexpr char kExports[] = "Looper/Export";
constexpr char kConfigPath[] = "Looper/config.bin";
static_assert(kChunk % kPeakFrames == 0, "load steps fill whole peak cells");

void put16(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
void put32(uint8_t* p, uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}
uint32_t get16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
uint32_t get32(const uint8_t* p) { return get16(p) | get16(p + 2) << 16; }

// 16-bit PCM WAV header at 44.1 kHz.
void wavHeader(uint8_t* h, uint32_t channels, uint32_t frames) {
    const uint32_t data = frames * channels * 2;
    std::memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + data);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 1);   // PCM
    put16(h + 22, channels);
    put32(h + 24, kSampleRate);
    put32(h + 28, kSampleRate * channels * 2);
    put16(h + 32, channels * 2);
    put16(h + 34, 16);
    std::memcpy(h + 36, "data", 4);
    put32(h + 40, data);
}

// Walks the chunks to "data"; leaves the file there. 16-bit PCM only.
bool parseWav(IStorage& fs, IStorage::Handle h, uint32_t& channels, uint32_t& dataBytes) {
    uint8_t b[16];
    if (fs.read(h, b, 12) != 12 || std::memcmp(b, "RIFF", 4) || std::memcmp(b + 8, "WAVE", 4)) return false;
    uint32_t pos = 12, bits = 0;
    channels = 0;
    for (int guard = 0; guard < 32; ++guard) {
        if (fs.read(h, b, 8) != 8) return false;
        pos += 8;
        const uint32_t size = get32(b + 4);
        if (!std::memcmp(b, "data", 4)) {
            dataBytes = size;
            return channels && bits == 16;
        }
        if (!std::memcmp(b, "fmt ", 4) && size >= 16) {
            if (fs.read(h, b, 16) != 16 || get16(b) != 1) return false;
            channels = get16(b + 2);
            bits = get16(b + 14);
            pos += 16;
            const uint32_t rest = size - 16 + (size & 1);
            if (rest && !fs.seek(h, pos + rest)) return false;
            pos += rest;
        } else {
            pos += size + (size & 1);
            if (!fs.seek(h, pos)) return false;
        }
    }
    return false;
}

inline uint8_t peak8(int v) {
    if (v < 0) v = -v;
    return uint8_t(v >> 7);
}

}  // namespace

void Files::init(Engine* engine, const Platform* p) {
    engine_ = engine;
    p_ = p;
    if (cardReady()) rescan();
}

bool Files::cardReady() const { return p_ && p_->storage && (!p_->storageReady || p_->storageReady()); }

// ---------------------------------------------------------------------------
// Settings

bool Files::loadConfig(Config& c) {
    if (!cardReady()) return false;
    IStorage& fs = *p_->storage;
    const IStorage::Handle h = fs.open(kConfigPath, IStorage::kRead);
    if (h < 0) return false;
    uint8_t b[14];
    const bool ok = fs.read(h, b, sizeof b) == int32_t(sizeof b) && !std::memcmp(b, "LCFG", 4) && get32(b + 4) == 1;
    fs.close(h);
    if (!ok) return false;
    c.brightness = b[8] > 2 ? 2 : b[8];
    c.pads = b[9] < 10 ? 10 : (b[9] > 100 ? 100 : b[9]);
    c.phones = b[10] > 100 ? 100 : b[10];
    c.source = b[11] == 2 || b[11] == 3 ? b[11] : 0;
    c.gain = b[12] > 100 ? 100 : b[12];
    c.monitor = b[13] ? 1 : 0;
    return true;
}

bool Files::saveConfig(const Config& c) {
    if (!cardReady()) return false;
    IStorage& fs = *p_->storage;
    fs.mkdir(kRoot);
    const IStorage::Handle h = fs.open(kConfigPath, IStorage::kWrite);
    if (h < 0) return false;
    uint8_t b[14];
    std::memcpy(b, "LCFG", 4);
    put32(b + 4, 1);
    b[8] = c.brightness;
    b[9] = c.pads;
    b[10] = c.phones;
    b[11] = c.source;
    b[12] = c.gain;
    b[13] = c.monitor;
    const bool ok = fs.write(h, b, sizeof b) == int32_t(sizeof b);
    fs.close(h);
    return ok;
}

// ---------------------------------------------------------------------------
// Projects

void Files::rescan() {
    count_ = 0;
    if (!cardReady()) return;
    auto add = [](const IStorage::DirEntry& e, void* ctx) {
        Files* f = static_cast<Files*>(ctx);
        // Numbered folders only: "001" .. "999".
        if (!e.isDir || std::strlen(e.name) != 3) return true;
        int n = 0;
        for (int i = 0; i < 3; ++i) {
            if (e.name[i] < '0' || e.name[i] > '9') return true;
            n = n * 10 + (e.name[i] - '0');
        }
        if (n && f->count_ < kMaxProjects) f->numbers_[f->count_++] = n;
        return true;
    };
    p_->storage->listDir(kProjects, add, this);
    for (int i = 1; i < count_; ++i)   // a few dozen entries: insertion sort
        for (int j = i; j > 0 && numbers_[j - 1] > numbers_[j]; --j) {
            const int x = numbers_[j];
            numbers_[j] = numbers_[j - 1];
            numbers_[j - 1] = x;
        }
}

bool Files::nextFolder(const char* parent, int& number, char* path, size_t n) {
    IStorage& fs = *p_->storage;
    fs.mkdir(kRoot);
    fs.mkdir(parent);
    for (int i = 1; i <= 999; ++i) {
        std::snprintf(path, n, "%s/%03d", parent, i);
        if (fs.exists(path)) continue;
        if (!fs.mkdir(path)) return false;
        number = i;
        return true;
    }
    return false;
}

void Files::snapshot(ProjectFile& pf) const {
    // Everything a project restores, read as the main loop sees it now.
    const Engine& e = *engine_;
    std::memset(&pf, 0, sizeof pf);
    std::memcpy(pf.magic, "LPRJ", 4);
    pf.version = 2;
    pf.bar = e.masterLen();
    pf.bpm = e.opt(OptParam::kTempo);
    pf.beats = e.beatsPerBar();
    pf.metronome = e.opt(OptParam::kMetronome);
    pf.click = e.opt(OptParam::kClick);
    pf.threshold = e.opt(OptParam::kThreshold);
    pf.monitor = e.monitoring();
    pf.feedback = e.feedback();
    pf.master = e.mix(MixParam::kMaster);
    pf.ret[0] = e.mix(MixParam::kReturn1);
    pf.ret[1] = e.mix(MixParam::kReturn2);
    for (int s = 0; s < kFxSlots; ++s) {
        pf.fxType[s] = uint8_t(e.fxType(s));
        for (int k = 0; k < kFxBank; ++k) {
            pf.fxKnob[s][k] = e.fxKnob(s, k);
            pf.fxKnob2[s][k] = e.fxKnob(s, kFxBank + k);
        }
    }
    for (int i = 0; i < kPerfFxCount; ++i) pf.perf[i] = e.perfParam(PerfFx(i));
    for (int t = 0; t < kTracks; ++t) {
        const TrackView& v = e.view(t);
        ProjectTrack& pt = pf.track[t];
        const TrackState s = TrackState(v.state.load());
        const bool loop = v.len.load() && (s == TrackState::kPlaying || s == TrackState::kOverdubbing ||
                                           s == TrackState::kStopped || s == TrackState::kQueued);
        pt.len = loop ? v.len.load() : 0;
        const uint32_t bar = e.barFrames();
        pt.phase = bar ? (bar - v.barOffset.load() % bar) % bar : 0;
        pt.mono = v.mono.load();
        pt.stopped = s == TrackState::kStopped || s == TrackState::kQueued;
        pt.mode = uint8_t(e.mode(t));
        pt.volume = e.volume(t);
        pt.muted = v.muted.load();
        pt.send1 = e.mix(MixParam::kSend1, t);
        pt.send2 = e.mix(MixParam::kSend2, t);
        pt.stop = e.opt(OptParam::kStop, t);
        pt.lenBeats = e.opt(OptParam::kLength, t);
        pt.start = e.opt(OptParam::kStart, t);
        pt.input = e.opt(OptParam::kInput, t);
        pt.after = e.opt(OptParam::kAfter, t);
        pt.stretch = e.opt(OptParam::kStretch, t);
        for (int g = 0; g < kGrainKnobs; ++g) pf.grain[t][g] = e.grainParam(t, g);
    }
}

void Files::postSettings(const ProjectFile& pf, bool tracks) {
    // Into the engine's queue, in order. First (tracks = false) the settings
    // that must be in before the audio (tempo, options, effects); then the
    // loops themselves and their mixer settings.
    Engine& e = *engine_;
    auto opt = [&](int t, OptParam p, int v) { e.post(Engine::kOpt, uint8_t(t), int16_t((int(p) << 8) | (v & 0xFF))); };
    auto mix = [&](int t, MixParam p, int v) { e.post(Engine::kMix, uint8_t(t), int16_t((int(p) << 8) | (v & 0xFF))); };
    if (!tracks) {
        opt(0, OptParam::kTempo, pf.bpm);
        opt(0, OptParam::kBeats, pf.beats);
        opt(0, OptParam::kMetronome, pf.metronome);
        opt(0, OptParam::kClick, pf.click);
        opt(0, OptParam::kThreshold, pf.threshold);
        e.post(Engine::kMonitor, 0, pf.monitor);
        e.post(Engine::kFeedback, 0, pf.feedback);
        mix(0, MixParam::kMaster, pf.master);
        mix(0, MixParam::kReturn1, pf.ret[0]);
        mix(0, MixParam::kReturn2, pf.ret[1]);
        for (int s = 0; s < kFxSlots; ++s) {
            e.post(Engine::kFxType, uint8_t(s), pf.fxType[s] < kFxTypeCount ? pf.fxType[s] : 0);
            for (int k = 0; k < kFxBank; ++k) {
                e.post(Engine::kFxKnob, uint8_t(s), int16_t((k << 8) | pf.fxKnob[s][k]));
                if (pf.version >= 2)
                    e.post(Engine::kFxKnob, uint8_t(s), int16_t(((kFxBank + k) << 8) | pf.fxKnob2[s][k]));
            }
        }
        for (int i = 0; i < kPerfFxCount; ++i) e.post(Engine::kPerfParam, 0, int16_t((i << 8) | pf.perf[i]));
        for (int t = 0; t < kTracks; ++t) {
            const ProjectTrack& pt = pf.track[t];
            opt(t, OptParam::kStop, pt.stop);
            opt(t, OptParam::kLength, pt.lenBeats);
            opt(t, OptParam::kStart, pt.start);
            opt(t, OptParam::kInput, pt.input);
            opt(t, OptParam::kAfter, pt.after);
            opt(t, OptParam::kStretch, pt.stretch);
            if (pf.version >= 2)
                for (int g = 0; g < kGrainKnobs; ++g)
                    e.post(Engine::kGrainParam, uint8_t(t), int16_t((g << 8) | pf.grain[t][g]));
        }
        return;
    }
    if (pf.bar) {
        e.post(Engine::kSetBar, 0, 0, pf.bar);
        opt(0, OptParam::kTempo, pf.bpm);   // with the bar in, the saved tempo is a tape speed
    }
    for (int t = 0; t < kTracks; ++t) {
        const ProjectTrack& pt = pf.track[t];
        mix(t, MixParam::kLevel, pt.volume);
        mix(t, MixParam::kSend1, pt.send1);
        mix(t, MixParam::kSend2, pt.send2);
        e.post(Engine::kSetMute, uint8_t(t), pt.muted);
        if (!pt.len || !((loaded_ >> t) & 1)) continue;
        e.post(Engine::kInstall, uint8_t(t), int16_t((pt.mono ? 1 : 0) | (pt.stopped ? 2 : 0)), pt.len, pt.phase);
        e.post(Engine::kMode, uint8_t(t), pt.mode);
    }
}

bool Files::save(bool asNew) {
    if (busy() || !cardReady()) return false;
    IStorage& fs = *p_->storage;
    int number = current_;
    if (asNew || !current_) {
        if (!nextFolder(kProjects, number, dir_, sizeof dir_)) {
            fail("Could not make a project folder");
            return false;
        }
    } else {
        std::snprintf(dir_, sizeof dir_, "%s/%03d", kProjects, number);
        fs.mkdir(kRoot);
        fs.mkdir(kProjects);
        fs.mkdir(dir_);
    }
    number_ = number;
    snapshot(pf_);
    char path[80];   // dir_ (48) + "/track1.wav"
    std::snprintf(path, sizeof path, "%s/project.bin", dir_);
    const IStorage::Handle h = fs.open(path, IStorage::kWrite);
    if (h < 0 || fs.write(h, &pf_, sizeof pf_) != int32_t(sizeof pf_)) {
        if (h >= 0) fs.close(h);
        fail("Could not write the project");
        return false;
    }
    fs.close(h);
    todo_ = 0;
    jobFrames_ = jobDone_ = 0;
    for (int t = 0; t < kTracks; ++t) {
        std::snprintf(path, sizeof path, "%s/track%d.wav", dir_, t + 1);
        if (pf_.track[t].len) {
            todo_ |= uint8_t(1u << t);
            jobFrames_ += pf_.track[t].len;
        } else if (fs.exists(path)) {
            fs.remove(path);   // an older save had audio here
        }
    }
    kind_ = kSave;
    h_ = -1;
    std::snprintf(status_, sizeof status_, "Saving project %03d...", number_);
    return true;
}

bool Files::exportTracks(uint8_t mask) {
    if (busy() || !cardReady()) return false;
    todo_ = 0;
    jobFrames_ = jobDone_ = 0;
    for (int t = 0; t < kTracks; ++t) {
        const uint32_t len = engine_->view(t).len.load();
        if (((mask >> t) & 1) && len) {
            todo_ |= uint8_t(1u << t);
            jobFrames_ += len;
            pf_.track[t].len = len;   // the export writes what is there now
        }
    }
    if (!todo_) {
        std::snprintf(status_, sizeof status_, "Nothing to export: no loop");
        return false;
    }
    if (!nextFolder(kExports, number_, dir_, sizeof dir_)) {
        fail("Could not make an export folder");
        return false;
    }
    kind_ = kExport;
    h_ = -1;
    std::snprintf(status_, sizeof status_, "Exporting...");
    return true;
}

bool Files::load(int number) {
    if (busy() || !cardReady()) return false;
    IStorage& fs = *p_->storage;
    std::snprintf(dir_, sizeof dir_, "%s/%03d", kProjects, number);
    char path[80];   // dir_ (48) + "/track1.wav"
    std::snprintf(path, sizeof path, "%s/project.bin", dir_);
    const IStorage::Handle h = fs.open(path, IStorage::kRead);
    std::memset(&pf_, 0, sizeof pf_);
    const int32_t got = h >= 0 ? fs.read(h, &pf_, sizeof pf_) : -1;
    const bool ok = !std::memcmp(pf_.magic, "LPRJ", 4) &&
                    ((pf_.version == 1 && got == int32_t(kProjectV1Bytes)) ||
                     (pf_.version == 2 && got == int32_t(sizeof pf_)));
    if (h >= 0) fs.close(h);
    if (!ok) {
        fail("Not a looper project");
        return false;
    }
    number_ = number;
    jobFrames_ = jobDone_ = 0;
    for (const ProjectTrack& pt : pf_.track) jobFrames_ += pt.len;
    kind_ = kLoad;
    phase_ = 0;
    h_ = -1;
    loaded_ = 0;
    std::snprintf(status_, sizeof status_, "Loading project %03d...", number_);
    return true;
}

// ---------------------------------------------------------------------------
// Jobs

bool Files::openWav(int t) {
    // Save / export: track t from its loop start, as recorded (mono or stereo).
    const TrackView& v = engine_->view(t);
    track_ = t;
    mono_ = v.mono.load();
    start_ = v.start.load();
    total_ = pf_.track[t].len;
    const uint32_t len = v.len.load();
    if (total_ > len) total_ = len;   // trimmed or cut since the snapshot
    done_ = 0;
    char path[80];   // dir_ (48) + "/track1.wav"
    std::snprintf(path, sizeof path, "%s/track%d.wav", dir_, t + 1);
    h_ = p_->storage->open(path, IStorage::kWrite);
    if (h_ < 0) return false;
    uint8_t hdr[44];
    wavHeader(hdr, mono_ ? 1 : 2, total_);
    return p_->storage->write(h_, hdr, sizeof hdr) == int32_t(sizeof hdr);
}

bool Files::writeStep() {
    const uint32_t n = total_ - done_ < kChunk ? total_ - done_ : kChunk;
    const uint32_t fb = mono_ ? 2 : 4;
    const int16_t* src = engine_->memory(track_).frames + (mono_ ? size_t(start_ + done_) : size_t(start_ + done_) * 2);
    if (n && p_->storage->write(h_, src, n * fb) != int32_t(n * fb)) return false;
    done_ += n;
    jobDone_ += n;
    return true;
}

bool Files::readStep() {
    // Load: the next chunk of track_'s WAV straight into its (empty) buffer,
    // and the peak cells it fills.
    const TrackMemory& m = engine_->memory(track_);
    const uint32_t n = total_ - done_ < kChunk ? total_ - done_ : kChunk;
    const uint32_t fb = mono_ ? 2 : 4;
    int16_t* dst = m.frames + (mono_ ? size_t(done_) : size_t(done_) * 2);
    if (p_->storage->read(h_, dst, n * fb) != int32_t(n * fb)) return false;
    for (uint32_t c = done_ / kPeakFrames; c * kPeakFrames < done_ + n; ++c) {
        uint8_t pl = 0, pr = 0;
        const uint32_t f1 = (c + 1) * kPeakFrames < done_ + n ? (c + 1) * kPeakFrames : done_ + n;
        for (uint32_t f = c * kPeakFrames; f < f1; ++f) {
            if (mono_) {
                const uint8_t p = peak8(m.frames[f]);
                if (p > pl) pl = p;
            } else {
                const uint8_t a = peak8(m.frames[2 * f]), b = peak8(m.frames[2 * f + 1]);
                if (a > pl) pl = a;
                if (b > pr) pr = b;
            }
        }
        if (mono_) m.peaks[c] = pl;
        else {
            m.peaks[2 * c] = pl;
            m.peaks[2 * c + 1] = pr;
        }
    }
    done_ += n;
    jobDone_ += n;
    return true;
}

void Files::service() {
    if (kind_ == kNone) return;
    IStorage& fs = *p_->storage;
    if (!cardReady()) {
        fail("The SD card went away");
        return;
    }
    if (kind_ == kSave || kind_ == kExport) {
        if (h_ < 0) {
            int t = 0;
            while (t < kTracks && !((todo_ >> t) & 1)) ++t;
            if (t == kTracks) {
                char msg[64];
                if (kind_ == kSave) {
                    current_ = number_;
                    std::snprintf(msg, sizeof msg, "Saved project %03d", number_);
                } else {
                    std::snprintf(msg, sizeof msg, "Exported to %s", dir_);
                }
                finish(msg);
                return;
            }
            if (!openWav(t)) {
                fail("Could not write a WAV file");
                return;
            }
        }
        if (!writeStep()) {
            fail("Writing to the card failed");
            return;
        }
        if (done_ >= total_) {
            fs.close(h_);
            h_ = -1;
            todo_ &= uint8_t(~(1u << track_));
        }
        return;
    }
    // Load
    switch (phase_) {
        case 0:   // clear everything (the transport stops with it)
            engine_->post(Engine::kClearAll);
            phase_ = 1;
            return;
        case 1:   // wait until every buffer is zeroed and the tracks are empty
            for (int t = 0; t < kTracks; ++t)
                if (TrackState(engine_->view(t).state.load()) != TrackState::kEmpty) return;
            postSettings(pf_, false);
            todo_ = 0;
            for (int t = 0; t < kTracks; ++t)
                if (pf_.track[t].len) todo_ |= uint8_t(1u << t);
            phase_ = 2;
            return;
        case 2: {
            if (h_ < 0) {
                int t = 0;
                while (t < kTracks && !((todo_ >> t) & 1)) ++t;
                if (t == kTracks) {
                    postSettings(pf_, true);
                    current_ = number_;
                    char msg[64];
                    std::snprintf(msg, sizeof msg, "Loaded project %03d", number_);
                    finish(msg);
                    return;
                }
                // Open the WAV and check it matches the project.
                char path[80];
                std::snprintf(path, sizeof path, "%s/track%d.wav", dir_, t + 1);
                track_ = t;
                mono_ = pf_.track[t].mono != 0;
                done_ = 0;
                h_ = fs.open(path, IStorage::kRead);
                uint32_t ch = 0, bytes = 0;
                const uint32_t fb = mono_ ? 2 : 4;
                const uint32_t cap = mono_ ? 2 * engine_->memory(t).capacity : engine_->memory(t).capacity;
                if (h_ < 0 || !parseWav(fs, h_, ch, bytes) || ch != (mono_ ? 1u : 2u)) {
                    if (h_ >= 0) fs.close(h_);
                    h_ = -1;
                    todo_ &= uint8_t(~(1u << t));   // skip this track; the rest still loads
                    std::snprintf(status_, sizeof status_, "Track %d: unreadable WAV, skipped", t + 1);
                    return;
                }
                total_ = pf_.track[t].len;
                if (total_ > bytes / fb) total_ = bytes / fb;
                if (total_ > cap) total_ = cap;
                pf_.track[t].len = total_;
            }
            if (!readStep()) {
                fs.close(h_);
                h_ = -1;
                todo_ &= uint8_t(~(1u << track_));
                std::snprintf(status_, sizeof status_, "Track %d: read error, skipped", track_ + 1);
                return;
            }
            if (done_ >= total_) {
                fs.close(h_);
                h_ = -1;
                todo_ &= uint8_t(~(1u << track_));
                loaded_ |= uint8_t(1u << track_);
            }
            return;
        }
        default: return;
    }
}

float Files::progress() const { return jobFrames_ ? float(jobDone_) / float(jobFrames_) : 0.0f; }

void Files::finish(const char* msg) {
    kind_ = kNone;
    h_ = -1;
    std::snprintf(status_, sizeof status_, "%s", msg);
    rescan();
}

void Files::fail(const char* msg) {
    if (h_ >= 0 && p_ && p_->storage) p_->storage->close(h_);
    h_ = -1;
    kind_ = kNone;
    std::snprintf(status_, sizeof status_, "%s", msg);
}

}  // namespace looper
