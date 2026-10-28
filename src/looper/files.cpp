#include "looper/files.h"

#include "looper/stream.h"

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
constexpr char kConfigTmp[] = "Looper/config.tmp";   // written first, then renamed over config.bin
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

// Walks the chunks to "data"; leaves the file there. 16-bit PCM only; rate is
// the file's sample rate (the caller checks it).
bool parseWav(IStorage& fs, IStorage::Handle h, uint32_t& channels, uint32_t& rate, uint32_t& dataBytes) {
    uint8_t b[16];
    if (fs.read(h, b, 12) != 12 || std::memcmp(b, "RIFF", 4) || std::memcmp(b + 8, "WAVE", 4)) return false;
    uint32_t pos = 12, bits = 0;
    channels = rate = 0;
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
            rate = get32(b + 4);
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

// A numbered folder's number ("001" .. "999"), 0 = not one.
int folderNumber(const IStorage::DirEntry& e) {
    if (!e.isDir || std::strlen(e.name) != 3) return 0;
    int n = 0;
    for (int i = 0; i < 3; ++i) {
        if (e.name[i] < '0' || e.name[i] > '9') return 0;
        n = n * 10 + (e.name[i] - '0');
    }
    return n;
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
    IStorage::Handle h = fs.open(kConfigPath, IStorage::kRead);
    if (h < 0) h = fs.open(kConfigTmp, IStorage::kRead);   // a save cut off between its remove and rename
    if (h < 0) return false;
    // Version 1: 14 bytes. Version 2 (15 + 2 a bytes): the MIDI clock, then
    // the a MIDI actions. Version 3 and later (18 + 2 a bytes, maybe more):
    // the MIDI clock, the latency, the number of actions, the actions; a
    // later version only adds to the end, so its file still reads here.
    uint8_t b[64] = {};
    const int32_t got = fs.read(h, b, sizeof b);
    fs.close(h);
    const uint32_t ver = got >= 8 ? get32(b + 4) : 0;
    if (got < 14 || std::memcmp(b, "LCFG", 4) || ver < 1 || (ver == 1 && got != 14)) return false;
    int map = 0, count = 0;   // where the MIDI actions start, and how many there are
    if (ver == 2) {
        map = 15;
        count = (got - 15) / 2;
    } else if (ver >= 3) {
        if (got < 18) return false;
        c.latency = uint16_t(b[15] | b[16] << 8);
        if (c.latency > kMaxLatencyFrames) c.latency = 0;
        map = 18;
        count = b[17];
        if (count > (got - 18) / 2) count = (got - 18) / 2;
    }
    if (ver >= 2) {
        c.midiClock = b[14] > 2 ? 0 : b[14];
        for (int a = 0; a < kMidiActions && a < count; ++a) {
            c.midiMap[a].type = b[map + 2 * a] > 2 ? 0 : b[map + 2 * a];
            c.midiMap[a].num = b[map + 1 + 2 * a] & 0x7F;
        }
    }
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
    // To config.tmp, then in place of config.bin: a power cut leaves one whole
    // file or the other, never a short config.bin.
    const IStorage::Handle h = fs.open(kConfigTmp, IStorage::kWrite);
    if (h < 0) return false;
    static_assert(18 + 2 * kMidiActions <= 64, "loadConfig reads 64 bytes");
    uint8_t b[18 + 2 * kMidiActions];
    std::memcpy(b, "LCFG", 4);
    put32(b + 4, 3);
    b[8] = c.brightness;
    b[9] = c.pads;
    b[10] = c.phones;
    b[11] = c.source;
    b[12] = c.gain;
    b[13] = c.monitor;
    b[14] = c.midiClock;
    b[15] = uint8_t(c.latency);
    b[16] = uint8_t(c.latency >> 8);
    b[17] = uint8_t(kMidiActions);
    for (int a = 0; a < kMidiActions; ++a) {
        b[18 + 2 * a] = c.midiMap[a].type;
        b[19 + 2 * a] = c.midiMap[a].num;
    }
    const bool ok = fs.write(h, b, sizeof b) == int32_t(sizeof b);
    fs.close(h);
    if (!ok || (fs.exists(kConfigPath) && !fs.remove(kConfigPath))) return false;
    return fs.rename(kConfigTmp, kConfigPath);
}

// ---------------------------------------------------------------------------
// Projects

void Files::rescan() {
    count_ = 0;
    std::memset(nameRead_, 0, sizeof nameRead_);
    std::memset(projName_, 0, sizeof projName_);
    if (!cardReady()) return;
    auto add = [](const IStorage::DirEntry& e, void* ctx) {
        Files* f = static_cast<Files*>(ctx);
        // Numbered folders only; past kMaxProjects the newest (highest) stay,
        // whatever order the card lists them in.
        const int n = folderNumber(e);
        if (!n) return true;
        if (f->count_ < kMaxProjects) {
            f->numbers_[f->count_++] = n;
            return true;
        }
        int lo = 0;
        for (int i = 1; i < f->count_; ++i)
            if (f->numbers_[i] < f->numbers_[lo]) lo = i;
        if (n > f->numbers_[lo]) f->numbers_[lo] = n;
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
    // The number after the highest there, so the newest is always last (a
    // gap left by a deleted folder is not reused).
    IStorage& fs = *p_->storage;
    fs.mkdir(kRoot);
    fs.mkdir(parent);
    int high = 0;
    auto top = [](const IStorage::DirEntry& e, void* ctx) {
        int& h = *static_cast<int*>(ctx);
        const int k = folderNumber(e);
        if (k > h) h = k;
        return true;
    };
    if (fs.listDir(parent, top, &high) < 0) {
        fail("Could not read the card");
        return false;
    }
    if (high >= 999) {
        fail("Folder 999 is used: move folders off the card");
        return false;
    }
    std::snprintf(path, n, "%s/%03d", parent, high + 1);
    if (fs.exists(path) || !fs.mkdir(path)) {   // a file by that name
        fail("Could not make a folder");
        return false;
    }
    number = high + 1;
    return true;
}

void Files::snapshot(ProjectFile& pf) const {
    // Everything a project restores, read as the main loop sees it now.
    const Engine& e = *engine_;
    std::memset(&pf, 0, sizeof pf);
    std::memcpy(pf.magic, "LPRJ", 4);
    pf.version = 5;
    pf.accent = e.opt(OptParam::kAccent);
    pf.clickSound = e.opt(OptParam::kClickSound);
    pf.clickWhen = e.opt(OptParam::kClickWhen);
    pf.countIn = e.opt(OptParam::kCountInBars);
    for (int t = 0; t < kTracks; ++t) {
        pf.filter[t] = uint8_t(e.filterFine(t) / 10);
        pf.filterFine[t] = uint8_t(e.filterFine(t) % 10);
        for (int g = kGrainKnobsV2; g < kGrainKnobs; ++g) pf.grain2[t][g - kGrainKnobsV2] = e.grainParam(t, g);
        std::memcpy(pf.trackName[t], trackName_[t], kNameLen);
        pf.trackName[t][kNameLen - 1] = 0;
    }
    pf.stopFade = e.opt(OptParam::kStopFade);
    pf.monitorFx = e.opt(OptParam::kMonitorFx);
    pf.chain[0] = e.mix(MixParam::kChain1);
    pf.chain[1] = e.mix(MixParam::kChain2);
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
    for (int i = 0; i < kPerfFxCount; ++i) {
        pf.perf[i] = e.perfParam(0, PerfFx(i));
        for (int t = 0; t < kTracks; ++t) pf.perfTrack[t][i] = e.perfParam(t, PerfFx(i));
    }
    pf.busMute = uint8_t((e.busMuted(0) ? 1 : 0) | (e.busMuted(1) ? 2 : 0) | (e.busMuted(2) ? 4 : 0));
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
        for (int g = 0; g < kGrainKnobsV2; ++g) pf.grain[t][g] = e.grainParam(t, g);
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
        // (An older project's missing fields hold the defaults: load.)
        opt(0, OptParam::kStopFade, pf.stopFade);
        opt(0, OptParam::kMonitorFx, pf.monitorFx);
        opt(0, OptParam::kAccent, pf.accent);
        opt(0, OptParam::kClickSound, pf.clickSound);
        opt(0, OptParam::kClickWhen, pf.clickWhen);
        opt(0, OptParam::kCountInBars, pf.countIn);
        mix(0, MixParam::kChain1, pf.chain[0]);
        mix(0, MixParam::kChain2, pf.chain[1]);
        e.post(Engine::kMonitor, 0, pf.monitor);
        e.post(Engine::kFeedback, 0, pf.feedback);
        mix(0, MixParam::kMaster, pf.master);
        mix(0, MixParam::kReturn1, pf.ret[0]);
        mix(0, MixParam::kReturn2, pf.ret[1]);
        for (int s = 0; s < kFxSlots; ++s) {
            e.post(Engine::kFxType, uint8_t(s), pf.fxType[s] < kFxTypeCount ? pf.fxType[s] : 0);
            for (int k = 0; k < kFxBank; ++k) {
                e.post(Engine::kFxKnob, uint8_t(s), int16_t((k << 8) | pf.fxKnob[s][k]));
                e.post(Engine::kFxKnob, uint8_t(s), int16_t(((kFxBank + k) << 8) | pf.fxKnob2[s][k]));
            }
        }
        for (int t = 0; t < kTracks; ++t)
            for (int i = 0; i < kPerfFxCount; ++i)
                e.post(Engine::kPerfParam, uint8_t(t), int16_t((i << 8) | pf.perfTrack[t][i]));
        for (int b = 0; b < 3; ++b) e.post(Engine::kBusMute, uint8_t(b), int16_t((pf.busMute >> b) & 1));
        for (int t = 0; t < kTracks; ++t) {
            const ProjectTrack& pt = pf.track[t];
            opt(t, OptParam::kStop, pt.stop);
            opt(t, OptParam::kLength, pt.lenBeats);
            opt(t, OptParam::kStart, pt.start);
            opt(t, OptParam::kInput, pt.input);
            opt(t, OptParam::kAfter, pt.after);
            opt(t, OptParam::kStretch, pt.stretch);
            for (int g = 0; g < kGrainKnobsV2; ++g)
                e.post(Engine::kGrainParam, uint8_t(t), int16_t((g << 8) | pf.grain[t][g]));
            for (int g = kGrainKnobsV2; g < kGrainKnobs; ++g)
                e.post(Engine::kGrainParam, uint8_t(t), int16_t((g << 8) | pf.grain2[t][g - kGrainKnobsV2]));
            e.post(Engine::kFilterFine, uint8_t(t),
                   int16_t((pf.filter[t] > 100 ? 100 : pf.filter[t]) * 10 + (pf.filterFine[t] < 10 ? pf.filterFine[t] : 0)));
            std::memcpy(trackName_[t], pf.trackName[t], kNameLen);
            trackName_[t][kNameLen - 1] = 0;
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
        e.post(Engine::kInstall, uint8_t(t),
               int16_t((pt.mono ? 1 : 0) | (pt.stopped ? 2 : 0) | (((long_ >> t) & 1) ? 4 : 0)), pt.len, pt.phase);
        e.post(Engine::kMode, uint8_t(t), pt.mode);
    }
}

bool Files::save(bool asNew) {
    if (busy() || !cardReady()) return false;
    if (engine_->editBusy()) {   // a cut or undo is moving audio: the save would catch it halfway
        std::snprintf(status_, sizeof status_, "An edit is running: save again when it is done");
        return false;
    }
    IStorage& fs = *p_->storage;
    int number = current_;
    if (asNew || !current_) {
        if (!nextFolder(kProjects, number, dir_, sizeof dir_)) return false;
    } else {
        std::snprintf(dir_, sizeof dir_, "%s/%03d", kProjects, number);
        fs.mkdir(kRoot);
        fs.mkdir(kProjects);
        fs.mkdir(dir_);
    }
    number_ = number;
    snapshot(pf_);
    // The new save goes to temporary files; the old one is not touched until
    // they are all written (commit). Leftovers of a save that failed go first.
    char path[80];   // dir_ (48) + "/project.tmp"
    std::snprintf(path, sizeof path, "%s/project.tmp", dir_);
    if (fs.exists(path)) fs.remove(path);
    todo_ = 0;
    jobFrames_ = jobDone_ = 0;
    for (int t = 0; t < kTracks; ++t) {
        std::snprintf(path, sizeof path, "%s/track%d.tmp", dir_, t + 1);
        if (pf_.track[t].len) {
            todo_ |= uint8_t(1u << t);
            jobFrames_ += pf_.track[t].len;
        } else if (fs.exists(path)) {
            fs.remove(path);
        }
    }
    kind_ = kSave;
    phase_ = 0;
    partial_ = false;
    h_ = -1;
    std::snprintf(status_, sizeof status_, "Saving project %03d...", number_);
    return true;
}

bool Files::commit(const ProjectFile& pf) {
    // Every trackN.tmp and project.tmp is written. Once project.bin is gone,
    // project.tmp says what the folder holds: a load that finds it so (a
    // power cut in here) runs this again to finish. Each step can run twice.
    IStorage& fs = *p_->storage;
    char path[80], tmp[80];
    std::snprintf(path, sizeof path, "%s/project.bin", dir_);
    if (fs.exists(path) && !fs.remove(path)) return false;
    for (int t = 0; t < kTracks; ++t) {
        std::snprintf(path, sizeof path, "%s/track%d.wav", dir_, t + 1);
        std::snprintf(tmp, sizeof tmp, "%s/track%d.tmp", dir_, t + 1);
        if (pf.track[t].len && !fs.exists(tmp)) continue;   // renamed already
        if (fs.exists(path) && !fs.remove(path)) return false;   // the old take, or one emptied since
        if (pf.track[t].len ? !fs.rename(tmp, path) : (fs.exists(tmp) && !fs.remove(tmp))) return false;
    }
    std::snprintf(path, sizeof path, "%s/project.bin", dir_);
    std::snprintf(tmp, sizeof tmp, "%s/project.tmp", dir_);
    return fs.rename(tmp, path);
}

void Files::tidy() {
    // A save that failed before its commit: the old save is as it was; its
    // temporary files go, and the folder too if it was a new one (empty).
    IStorage& fs = *p_->storage;
    char path[80];
    for (int t = 0; t < kTracks; ++t) {
        std::snprintf(path, sizeof path, "%s/track%d.tmp", dir_, t + 1);
        if (fs.exists(path)) fs.remove(path);
    }
    std::snprintf(path, sizeof path, "%s/project.tmp", dir_);
    if (fs.exists(path)) fs.remove(path);
    fs.remove(dir_);   // only when empty
}

bool Files::exportTracks(uint8_t mask) {
    if (busy() || !cardReady()) return false;
    if (engine_->editBusy()) {
        std::snprintf(status_, sizeof status_, "An edit is running: export again when it is done");
        return false;
    }
    todo_ = 0;
    skipped_ = 0;
    jobFrames_ = jobDone_ = 0;
    for (int t = 0; t < kTracks; ++t) {
        const TrackView& v = engine_->view(t);
        const uint32_t len = v.len.load();
        if (((mask >> t) & 1) && len) {
            todo_ |= uint8_t(1u << t);
            jobFrames_ += len;
            pf_.track[t].len = len;   // the export writes what is there now
            pf_.track[t].mono = v.mono.load();
        }
    }
    if (!todo_) {
        std::snprintf(status_, sizeof status_, "Nothing to export: no loop");
        return false;
    }
    if (!nextFolder(kExports, number_, dir_, sizeof dir_)) return false;
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
    auto readProject = [&](const char* name) {
        std::snprintf(path, sizeof path, "%s/%s", dir_, name);
        const IStorage::Handle h = fs.open(path, IStorage::kRead);
        std::memset(&pf_, 0, sizeof pf_);
        const int32_t got = h >= 0 ? fs.read(h, &pf_, sizeof pf_) : -1;
        if (h >= 0) fs.close(h);
        return !std::memcmp(pf_.magic, "LPRJ", 4) &&
               ((pf_.version == 1 && got == int32_t(kProjectV1Bytes)) ||
                (pf_.version == 2 && got == int32_t(kProjectV2Bytes)) ||
                (pf_.version == 3 && got == int32_t(kProjectV3Bytes)) ||
                (pf_.version == 4 && got == int32_t(kProjectV4Bytes)) ||
                (pf_.version == 5 && got == int32_t(sizeof pf_)));
    };
    // No project.bin but a project.tmp: a save was cut off in its commit;
    // finish it first.
    if (!fs.exists(path) && readProject("project.tmp") && !commit(pf_)) {
        fail("Could not finish this project's last save");
        return false;
    }
    if (!readProject("project.bin")) {
        fail("Not a looper project");
        return false;
    }
    // What an older version has no field for: the defaults (not what the
    // session before had).
    if (pf_.version < 2) {
        for (int s = 0; s < kFxSlots; ++s) {
            const FxTypeInfo& ti = fxTypeInfo(FxType(pf_.fxType[s] < kFxTypeCount ? pf_.fxType[s] : 0));
            for (int k = 0; k < kFxBank; ++k) pf_.fxKnob2[s][k] = ti.knob[kFxBank + k].name ? ti.knob[kFxBank + k].def : 0;
        }
        for (int t = 0; t < kTracks; ++t)
            for (int g = 0; g < kGrainKnobsV2; ++g) pf_.grain[t][g] = kGrainKnobInfo[g].def;
    }
    if (pf_.version < 3) {
        pf_.stopFade = 0;
        pf_.monitorFx = 3;   // the default: both
        for (uint8_t& c : pf_.chain) c = 0;
    }
    if (pf_.version < 5)   // one set for every track
        for (int t = 0; t < kTracks; ++t) std::memcpy(pf_.perfTrack[t], pf_.perf, kPerfFxCount);
    if (pf_.version == 4) {
        // Version 4's Speed went Rev 2x .. 2x (0..5) and its direction knob
        // turned the grains alone: now Speed is 1/2, 1x, 2x and Direction
        // moves the playhead.
        for (int t = 0; t < kTracks; ++t) {
            uint8_t* g = pf_.grain2[t];
            const uint8_t s = g[kGrainSpeed - kGrainKnobsV2] < 6 ? g[kGrainSpeed - kGrainKnobsV2] : 4;
            g[kGrainSpeed - kGrainKnobsV2] = uint8_t(s < 3 ? 2 - s : s - 3);
            g[kGrainDir - kGrainKnobsV2] = s < 3 ? kGrainReverse : kGrainForward;
        }
    }
    if (pf_.version < 4) {
        for (int t = 0; t < kTracks; ++t) {
            pf_.filter[t] = kFilterOff;
            for (int g = kGrainKnobsV2; g < kGrainKnobs; ++g) pf_.grain2[t][g - kGrainKnobsV2] = kGrainKnobInfo[g].def;
            std::memset(pf_.trackName[t], 0, kNameLen);
        }
        pf_.accent = 1;
        pf_.clickSound = 0;
        pf_.clickWhen = 0;
        pf_.countIn = 1;
    }
    number_ = number;
    jobFrames_ = jobDone_ = 0;
    for (const ProjectTrack& pt : pf_.track) jobFrames_ += pt.len;
    kind_ = kLoad;
    phase_ = 0;
    h_ = -1;
    loaded_ = 0;
    long_ = 0;
    skipped_ = 0;
    partial_ = false;
    std::snprintf(status_, sizeof status_, "Loading project %03d...", number_);
    return true;
}

// ---------------------------------------------------------------------------
// Jobs

bool Files::openWav(int t) {
    // Save / export: track t from its loop start, as recorded (mono or stereo).
    // The take is pinned here (length, start, mono, long); takeOk checks every
    // step that it is still the one (the save's project.bin says what it was).
    const TrackView& v = engine_->view(t);
    track_ = t;
    mono_ = pf_.track[t].mono != 0;
    start_ = v.start.load();
    total_ = pf_.track[t].len;
    longTake_ = engine_->longTake(t);
    take_ = engine_->stream(t).take.load();
    done_ = 0;
    char path[80];   // dir_ (48) + "/track1.wav"
    std::snprintf(path, sizeof path, "%s/track%d.%s", dir_, t + 1, kind_ == kSave ? "tmp" : "wav");
    h_ = p_->storage->open(path, IStorage::kWrite);
    if (h_ < 0) return false;
    uint8_t hdr[44];
    wavHeader(hdr, mono_ ? 1 : 2, total_);
    return p_->storage->write(h_, hdr, sizeof hdr) == int32_t(sizeof hdr);
}

bool Files::takeOk(int t) const {
    // Not cleared, re-recorded, trimmed, cut, multiplied or being moved since
    // openWav: else the WAV would hold parts of two takes (or of no buffer).
    const TrackView& v = engine_->view(t);
    const TrackState s = TrackState(v.state.load());
    const bool loop = s == TrackState::kPlaying || s == TrackState::kOverdubbing || s == TrackState::kStopped ||
                      s == TrackState::kQueued;
    return loop && v.len.load() == total_ && v.start.load() == start_ && v.mono.load() == mono_ && !v.busy.load() &&
           engine_->longTake(t) == longTake_ && engine_->stream(t).take.load() == take_;
}

bool Files::writeStep() {
    const uint32_t n = total_ - done_ < kChunk ? total_ - done_ : kChunk;
    const uint32_t fb = mono_ ? 2 : 4;
    const int16_t* src = engine_->memory(track_).frames + (mono_ ? size_t(start_ + done_) : size_t(start_ + done_) * 2);
    if (longTake_) {   // a long take: RAM, ring and card, as it plays now
        if (!stream_ || !stream_->read(track_, start_ + done_, p_->scratch, n)) return false;
        src = reinterpret_cast<const int16_t*>(p_->scratch);
    }
    if (n && p_->storage->write(h_, src, n * fb) != int32_t(n * fb)) return false;
    done_ += n;
    jobDone_ += n;
    return true;
}

bool Files::readStep() {
    // Load: the next chunk of track_'s WAV straight into its (empty) buffer,
    // and the peak cells it fills.
    // A long take: its RAM part goes to RAM and to the card, the rest to the
    // card through the staging buffer (steps never straddle head: both are
    // whole multiples of a step).
    const TrackMemory& m = engine_->memory(track_);
    const uint32_t n = total_ - done_ < kChunk ? total_ - done_ : kChunk;
    const uint32_t fb = mono_ ? 2 : 4;
    const bool isLong = (long_ >> track_) & 1, card = isLong && done_ >= head_;
    int16_t* dst = card ? reinterpret_cast<int16_t*>(p_->scratch)
                        : m.frames + (mono_ ? size_t(done_) : size_t(done_) * 2);
    if (kind_ == kDup ? !stream_ || !stream_->read(src_, start_ + done_, reinterpret_cast<uint8_t*>(dst), n)
                      : p_->storage->read(h_, dst, n * fb) != int32_t(n * fb))
        return false;
    if (isLong && (!stream_ || !stream_->write(track_, mono_, done_, reinterpret_cast<const uint8_t*>(dst), n)))
        return false;
    for (uint32_t c = done_ / kPeakFrames; c * kPeakFrames < done_ + n; ++c) {
        uint8_t pl = 0, pr = 0;
        const uint32_t f1 = (c + 1) * kPeakFrames < done_ + n ? (c + 1) * kPeakFrames : done_ + n;
        for (uint32_t f = c * kPeakFrames; f < f1; ++f) {
            const uint32_t j = f - done_;   // into dst
            if (mono_) {
                const uint8_t p = peak8(dst[j]);
                if (p > pl) pl = p;
            } else {
                const uint8_t a = peak8(dst[2 * j]), b = peak8(dst[2 * j + 1]);
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

void Files::setTrackName(int t, const char* s) {
    if (t < 0 || t >= kTracks) return;
    std::snprintf(trackName_[t], kNameLen, "%s", s ? s : "");
}

void Files::readName() {
    for (int i = 0; i < count_; ++i) {
        if ((nameRead_[i >> 3] >> (i & 7)) & 1) continue;
        nameRead_[i >> 3] |= uint8_t(1u << (i & 7));
        char path[64];
        std::snprintf(path, sizeof path, "%s/%03d/name.txt", kProjects, numbers_[i]);
        const IStorage::Handle h = p_->storage->open(path, IStorage::kRead);
        if (h < 0) return;
        const int32_t n = p_->storage->read(h, projName_[i], kProjectNameLen - 1);
        p_->storage->close(h);
        projName_[i][n > 0 ? n : 0] = 0;
        for (char* c = projName_[i]; *c; ++c)   // one line, printable
            if (*c == '\n' || *c == '\r') *c = 0;
            else if (*c < 32 || *c > 126) *c = '?';
        return;
    }
}

bool Files::renameProject(int i, const char* name) {
    if (i < 0 || i >= count_ || !cardReady()) return false;
    char path[64];
    std::snprintf(path, sizeof path, "%s/%03d/name.txt", kProjects, numbers_[i]);
    IStorage& fs = *p_->storage;
    if (!name || !name[0]) {   // no name: the number alone
        if (fs.exists(path)) fs.remove(path);
        projName_[i][0] = 0;
        return true;
    }
    const IStorage::Handle h = fs.open(path, IStorage::kWrite);
    if (h < 0) return false;
    const int32_t n = int32_t(std::strlen(name));
    const bool ok = fs.write(h, name, uint32_t(n)) == n;
    fs.close(h);
    if (ok) std::snprintf(projName_[i], kProjectNameLen, "%s", name);
    return ok;
}

void Files::openSampleDir(const char* sub) {
    if (!sub) {   // up: the part before the last '/'
        char* s = std::strrchr(sampleDir_, '/');
        if (s) *s = 0;
        else sampleDir_[0] = 0;
    } else if (!sub[0]) {
        sampleDir_[0] = 0;
    } else if (std::strlen(sampleDir_) + std::strlen(sub) + 2 < sizeof sampleDir_) {
        if (sampleDir_[0]) std::strcat(sampleDir_, "/");
        std::strcat(sampleDir_, sub);
    }
    listSamples();
}

void Files::listSamples() {
    sampleCount_ = 0;
    if (!cardReady()) return;
    auto add = [](const IStorage::DirEntry& e, void* ctx) {
        Files* f = static_cast<Files*>(ctx);
        if (e.name[0] == '.' || f->sampleCount_ >= kMaxSamples || std::strlen(e.name) >= kSampleNameLen) return true;
        const size_t n = std::strlen(e.name);
        const bool wav = n > 4 && (!std::strcmp(e.name + n - 4, ".wav") || !std::strcmp(e.name + n - 4, ".WAV") ||
                                   !std::strcmp(e.name + n - 4, ".Wav"));
        if (!e.isDir && !wav) return true;
        if (e.isDir && !std::strcmp(e.name, "System Volume Information")) return true;
        std::snprintf(f->sampleName_[f->sampleCount_], kSampleNameLen, "%s", e.name);
        f->sampleIsDir_[f->sampleCount_] = e.isDir;
        ++f->sampleCount_;
        return true;
    };
    p_->storage->listDir(sampleDir_, add, this);
    // Folders first, then by name (case aside): an insertion sort of a few dozen.
    auto before = [&](int a, int b) {
        if (sampleIsDir_[a] != sampleIsDir_[b]) return sampleIsDir_[a];
        for (const char *x = sampleName_[a], *y = sampleName_[b];; ++x, ++y) {
            const char p = char(*x >= 'a' && *x <= 'z' ? *x - 32 : *x), q = char(*y >= 'a' && *y <= 'z' ? *y - 32 : *y);
            if (p != q || !p) return p < q;
        }
    };
    for (int i = 1; i < sampleCount_; ++i)
        for (int j = i; j > 0 && before(j, j - 1); --j) {
            char tmp[kSampleNameLen];
            std::memcpy(tmp, sampleName_[j], kSampleNameLen);
            std::memcpy(sampleName_[j], sampleName_[j - 1], kSampleNameLen);
            std::memcpy(sampleName_[j - 1], tmp, kSampleNameLen);
            const bool d = sampleIsDir_[j];
            sampleIsDir_[j] = sampleIsDir_[j - 1];
            sampleIsDir_[j - 1] = d;
        }
}

bool Files::loadSample(int t, int i) {
    if (busy() || !cardReady() || t < 0 || t >= kTracks || i < 0 || i >= sampleCount_ || sampleIsDir_[i]) return false;
    if (TrackState(engine_->view(t).state.load()) != TrackState::kEmpty) {
        std::snprintf(status_, sizeof status_, "Track %d holds a loop: clear it first", t + 1);
        return false;
    }
    std::snprintf(samplePath_, sizeof samplePath_, "%s%s%s", sampleDir_, sampleDir_[0] ? "/" : "", sampleName_[i]);
    kind_ = kSample;
    phase_ = 0;
    track_ = t;
    h_ = -1;
    loaded_ = long_ = skipped_ = 0;
    partial_ = false;
    jobFrames_ = jobDone_ = 0;
    waitMs_ = p_->clock ? p_->clock->millis() : 0;
    std::snprintf(status_, sizeof status_, "Loading %s...", sampleName_[i]);
    return true;
}

bool Files::sampleStep() {
    // Open and check the WAV (16-bit PCM, 44.1 kHz, mono or stereo); read it
    // into the track's buffer (past its RAM: on to the card, as a long take);
    // then the engine takes it in, stopped: its Play starts it on a bar line.
    // On an empty session it sets the bar, as a first take would.
    IStorage& fs = *p_->storage;
    const int t = track_;
    const uint32_t now = p_->clock ? p_->clock->millis() : 0;
    if (phase_ == 0) {
        if (TrackState(engine_->view(t).state.load()) != TrackState::kEmpty) {
            fail("The track was recorded meanwhile: not loaded");
            return false;
        }
        h_ = fs.open(samplePath_, IStorage::kRead);
        uint32_t ch = 0, rate = 0, bytes = 0;
        if (h_ < 0 || !parseWav(fs, h_, ch, rate, bytes) || (ch != 1 && ch != 2)) {
            fail("Not a WAV this loads (16-bit PCM, mono or stereo)");
            return false;
        }
        if (rate != kSampleRate) {
            fail("The WAV is not 44.1 kHz");
            return false;
        }
        mono_ = ch == 1;
        const uint32_t fb = mono_ ? 2 : 4;
        const uint32_t cap = mono_ ? 2 * engine_->memory(t).capacity : engine_->memory(t).capacity;
        head_ = engine_->headFor(t, mono_);
        const uint32_t cardCap = stream_ ? stream_->capacity(t, mono_) : 0;
        total_ = bytes / fb;
        if (total_ < kSampleRate / 20) {   // 50 ms
            fail("The WAV is too short");
            return false;
        }
        if (total_ > cap) {   // longer than its RAM: the rest to the card
            if (!head_ || !cardCap) {
                if (now - waitMs_ < 5000) {   // the stream file may not be open yet
                    fs.close(h_);
                    h_ = -1;
                    return true;
                }
                fail("Too long for the RAM, and the card is not ready");
                return false;
            }
            if (total_ > cardCap) total_ = cardCap;   // what fits: up to 5 minutes (10 mono)
            long_ = uint8_t(1u << t);
        }
        done_ = 0;
        jobFrames_ = total_;
        phase_ = 1;
        return true;
    }
    if (phase_ == 1) {
        if (TrackState(engine_->view(t).state.load()) != TrackState::kEmpty) {
            fail("The track was recorded meanwhile: not loaded");
            return false;
        }
        if (!readStep()) {
            fail("Reading the WAV failed");
            return false;
        }
        if (done_ < total_) return true;
        fs.close(h_);
        h_ = -1;
        if (!engine_->masterLen()) engine_->post(Engine::kSetBar, 0, 0, total_);
        engine_->post(Engine::kInstall, uint8_t(t), int16_t((mono_ ? 1 : 0) | 2 | (long_ ? 4 : 0)), total_, 0);
        phase_ = 2;
        waitMs_ = now;
        return true;
    }
    const TrackState s = TrackState(engine_->view(t).state.load());
    if (s == TrackState::kEmpty && now - waitMs_ < 1000) return true;
    if (s == TrackState::kEmpty) {
        fail("The track did not take the WAV");
        return false;
    }
    const char* base = std::strrchr(samplePath_, '/');
    char msg[sizeof status_];
    std::snprintf(msg, sizeof msg, "Track %d: %.27s (Play starts it)", t + 1, base ? base + 1 : samplePath_);
    finish(msg);
    return true;
}

bool Files::duplicateLong(int t) {
    const int d = t + 1;
    if (busy() || !cardReady() || !stream_ || t < 0 || d >= kTracks) return false;
    const TrackView& v = engine_->view(t);
    if (!engine_->longTake(t) || TrackState(engine_->view(d).state.load()) != TrackState::kEmpty) return false;
    // The source is pinned as a save pins it (takeOk): one that changes
    // meanwhile stops the copy, so it never holds parts of two takes.
    src_ = t;
    track_ = d;
    mono_ = v.mono.load();
    start_ = v.start.load();
    total_ = v.len.load();
    longTake_ = true;
    take_ = engine_->stream(t).take.load();
    const uint32_t cap = mono_ ? 2 * engine_->memory(d).capacity : engine_->memory(d).capacity;
    head_ = engine_->headFor(d, mono_);
    long_ = total_ > cap ? uint8_t(1u << d) : 0;
    if (!takeOk(t) || (long_ && (!head_ || total_ > stream_->capacity(d, mono_)))) {
        std::snprintf(status_, sizeof status_, "Track %d's card file is not ready: not duplicated", d + 1);
        return false;
    }
    kind_ = kDup;
    phase_ = 1;
    h_ = -1;
    done_ = 0;
    jobFrames_ = total_;
    jobDone_ = 0;
    std::snprintf(status_, sizeof status_, "Duplicating track %d to track %d...", t + 1, d + 1);
    return true;
}

void Files::dupStep() {
    // Copy the take a chunk a step (RAM, ring and card, as it plays) into the
    // empty track (its RAM part, its stream file, its peaks); then the engine
    // takes it in, with the source's place on the clock, mode and play state.
    const int d = track_;
    const uint32_t now = p_->clock ? p_->clock->millis() : 0;
    if (phase_ == 1) {
        if (TrackState(engine_->view(d).state.load()) != TrackState::kEmpty) {
            fail("The next track was recorded meanwhile: not duplicated");
            return;
        }
        if (!takeOk(src_)) {
            fail("The track changed meanwhile: not duplicated");
            return;
        }
        if (!readStep()) {
            fail("Reading the take from the card failed");
            return;
        }
        if (done_ < total_) return;
        if (!takeOk(src_)) {
            fail("The track changed meanwhile: not duplicated");
            return;
        }
        engine_->post(Engine::kInstall, uint8_t(d), int16_t((mono_ ? 1 : 0) | (long_ ? 4 : 0) | 8), total_, uint32_t(src_));
        phase_ = 2;
        waitMs_ = now;
        return;
    }
    const TrackView& v = engine_->view(d);
    const TrackState s = TrackState(v.state.load());
    if ((s == TrackState::kEmpty || v.len.load() != total_) && now - waitMs_ < 1000) return;
    if (s == TrackState::kEmpty || v.len.load() != total_) {
        fail("The next track did not take the copy");
        return;
    }
    char msg[sizeof status_];
    std::snprintf(msg, sizeof msg, "Duplicated track %d to track %d", src_ + 1, d + 1);
    kind_ = kNone;   // no rescan: nothing on the card's project list changed
    std::snprintf(status_, sizeof status_, "%s", msg);
}

void Files::service() {
    if (kind_ == kNone) {
        if (count_ && cardReady()) readName();   // idle: a project name a call
        return;
    }
    IStorage& fs = *p_->storage;
    if (kind_ == kSample) {
        if (!cardReady()) fail("The SD card went away");
        else sampleStep();
        return;
    }
    if (kind_ == kDup) {
        if (!cardReady()) fail("The SD card went away");
        else dupStep();
        return;
    }
    if (!cardReady()) {
        fail("The SD card went away");
        return;
    }
    const uint32_t now = p_->clock ? p_->clock->millis() : 0;
    if (kind_ == kSave || kind_ == kExport) {
        char msg[64];
        if (h_ < 0) {
            int t = 0;
            while (t < kTracks && !((todo_ >> t) & 1)) ++t;
            if (t == kTracks) {
                if (kind_ == kSave) {
                    // Every WAV is in: project.bin's new version, then the swap.
                    char path[80];
                    std::snprintf(path, sizeof path, "%s/project.tmp", dir_);
                    const IStorage::Handle h = fs.open(path, IStorage::kWrite);
                    const bool ok = h >= 0 && fs.write(h, &pf_, sizeof pf_) == int32_t(sizeof pf_);
                    if (h >= 0) fs.close(h);
                    if (!ok) {
                        fail("Could not write the project");
                        return;
                    }
                    phase_ = 1;   // the temporary files are the save now: kept if the commit stops
                    if (!commit(pf_)) {
                        fail("Could not finish the save: card error");
                        return;
                    }
                    current_ = number_;
                    std::snprintf(msg, sizeof msg, "Saved project %03d", number_);
                } else {
                    int k = 0;
                    while (k < kTracks && !((skipped_ >> k) & 1)) ++k;
                    if (k < kTracks) std::snprintf(msg, sizeof msg, "Exported %03d without track %d (it changed)", number_, k + 1);
                    else std::snprintf(msg, sizeof msg, "Exported to %s", dir_);
                }
                finish(msg);
                return;
            }
            if (!openWav(t)) {
                fail("Could not write a WAV file");
                return;
            }
        }
        // The take is checked before and after each chunk: one that changed
        // meanwhile (cleared, re-recorded, cut...) is not written half and half.
        const bool same = takeOk();
        if (same && !writeStep()) {
            fail("Writing to the card failed");
            return;
        }
        if (!same || !takeOk()) {
            if (kind_ == kSave) {   // the old save stays as it was
                std::snprintf(msg, sizeof msg, "Not saved: track %d changed while saving", track_ + 1);
                fail(msg);
                return;
            }
            fs.close(h_);   // export: this track's WAV goes, the others still go out
            h_ = -1;
            char path[80];
            std::snprintf(path, sizeof path, "%s/track%d.wav", dir_, track_ + 1);
            fs.remove(path);
            todo_ &= uint8_t(~(1u << track_));
            skipped_ |= uint8_t(1u << track_);
            jobDone_ += total_ - done_;
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
            current_ = 0;   // the loops are no longer the project before (F1 must not save over it)
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
            waitMs_ = now;
            return;
        case 2: {
            if (h_ < 0) {
                int t = 0;
                while (t < kTracks && !((todo_ >> t) & 1)) ++t;
                if (t == kTracks) {
                    postSettings(pf_, true);
                    phase_ = 3;
                    waitMs_ = now;
                    return;
                }
                track_ = t;
                mono_ = pf_.track[t].mono != 0;
                done_ = 0;
                const uint32_t fb = mono_ ? 2 : 4;
                const uint32_t cap = mono_ ? 2 * engine_->memory(t).capacity : engine_->memory(t).capacity;
                head_ = engine_->headFor(t, mono_);
                const uint32_t cardCap = stream_ ? stream_->capacity(t, mono_) : 0;
                // Longer than its RAM: the rest goes to the card, through the
                // track's stream file. Not open yet (just after start-up or a
                // card change): wait for it a while, then leave the track out;
                // never cut the take short (a save would lose the rest for good).
                if (pf_.track[t].len > cap && head_ && stream_ && !cardCap && p_->clock && now - waitMs_ < 5000)
                    return;
                // Open the WAV and check it matches the project.
                char path[80];
                std::snprintf(path, sizeof path, "%s/track%d.wav", dir_, t + 1);
                h_ = fs.open(path, IStorage::kRead);
                uint32_t ch = 0, rate = 0, bytes = 0;
                if (h_ < 0 || !parseWav(fs, h_, ch, rate, bytes) || ch != (mono_ ? 1u : 2u)) {
                    skip(t, "unreadable");
                    return;
                }
                if (rate != kSampleRate) {
                    skip(t, "not 44.1 kHz");
                    return;
                }
                total_ = pf_.track[t].len;
                if (total_ > bytes / fb) total_ = bytes / fb;
                if (!total_) {
                    skip(t, "no audio");
                    return;
                }
                if (total_ > cap) {
                    if (!head_ || !cardCap) {
                        skip(t, "long, card not ready");
                        return;
                    }
                    if (total_ > cardCap) {
                        skip(t, "too long");
                        return;
                    }
                    long_ |= uint8_t(1u << t);
                }
                pf_.track[t].len = total_;
            }
            if (!readStep()) {
                skip(track_, "read error");
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
        case 3: {   // the engine takes the loops in (kInstall) within a block or two
            uint8_t in = 0;
            for (int t = 0; t < kTracks; ++t) {
                const TrackView& v = engine_->view(t);
                const TrackState s = TrackState(v.state.load());
                if (((loaded_ >> t) & 1) && s != TrackState::kEmpty && s != TrackState::kClearing &&
                    v.len.load() == pf_.track[t].len)
                    in |= uint8_t(1u << t);
            }
            if (in != loaded_ && p_->clock && now - waitMs_ < 1000) return;
            for (int t = 0; t < kTracks; ++t)
                if (((loaded_ & ~in) >> t) & 1) skip(t, "not taken in");
            char msg[64];
            partial_ = skipped_ != 0;
            if (!skipped_) {
                current_ = number_;
                std::snprintf(msg, sizeof msg, "Loaded project %03d", number_);
            } else {
                // Not the whole project: current_ stays 0, so F1 saves a new
                // one instead of losing the tracks left out.
                char list[2 * kTracks + 1] = {};
                int n = 0, last = 0;
                for (int t = 0; t < kTracks; ++t)
                    if ((skipped_ >> t) & 1) {
                        list[2 * n] = char('1' + t);
                        list[2 * n + 1] = ' ';
                        ++n;
                        last = t;
                    }
                list[2 * n - 1] = 0;
                if (n == 1) std::snprintf(msg, sizeof msg, "Loaded %03d without track %d: %s", number_, last + 1, why_);
                else std::snprintf(msg, sizeof msg, "Loaded %03d without tracks %s", number_, list);
            }
            finish(msg);
            return;
        }
        default: return;
    }
}

void Files::skip(int t, const char* why) {
    // Load: track t stays empty (its buffer may hold part of it: the next take
    // records over it); the rest still loads.
    if (h_ >= 0 && t == track_) {
        p_->storage->close(h_);
        h_ = -1;
    }
    todo_ &= uint8_t(~(1u << t));
    loaded_ &= uint8_t(~(1u << t));
    skipped_ |= uint8_t(1u << t);
    why_ = why;
    std::snprintf(status_, sizeof status_, "Track %d: %s, skipped", t + 1, why);
}

uint8_t Files::lockedTracks() const {
    if (kind_ == kLoad) return (1u << kTracks) - 1;
    if (kind_ == kSample) return uint8_t(1u << track_);
    if (kind_ == kDup) return uint8_t((1u << track_) | (1u << src_));
    return kind_ == kNone ? 0 : todo_;   // save / export: the tracks not written yet
}

const char* Files::lockReason() const {
    return kind_ == kLoad ? "Loading a project: wait until it is done"
                          : kind_ == kSample ? "Loading a sample into this track: wait until it is done"
                          : kind_ == kDup ? "Duplicating this track: wait until it is done"
                          : kind_ == kSave ? "Saving this track: wait until it is done"
                                           : "Exporting this track: wait until it is done";
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
    if (kind_ == kSave && phase_ == 0 && cardReady()) tidy();
    if (kind_ == kLoad && phase_ > 0) partial_ = true;   // the tracks were cleared: current_ is 0 already
    kind_ = kNone;
    std::snprintf(status_, sizeof status_, "%s", msg);
}

}  // namespace looper
