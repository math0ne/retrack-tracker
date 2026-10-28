// SD card work: settings (Looper/config.bin), projects (Looper/Projects/NNN:
// project.bin + one WAV per track) and WAV exports (Looper/Export/NNN).
//
// Saving, loading and exporting are jobs: service() (main loop) does one
// small step per call, so the screen and pads keep going and the audio never
// waits. Saving writes trackN.tmp and project.tmp next to the old save, then
// swaps them in (commit): the old save stays whole until the new one is.
// Loading clears the tracks, reads the WAVs straight into their (empty)
// buffers with the waveform peaks, then hands them to the engine (kInstall)
// with every setting the project holds. A load that leaves a track out is no
// longer that project: F1 then saves a new one.
#pragma once

#include <cstdint>

#include "hal_api.h"
#include "looper/engine.h"
#include "looper/midi.h"
#include "looper/platform.h"

namespace looper {

class Streamer;

// Settings kept across power-offs (the Config page).
struct Config {
    uint8_t brightness = 2;   // 0 low, 1 medium, 2 high
    uint8_t pads = 100;       // pad LED brightness, 10..100 %
    uint8_t phones = 70;      // headphone level 0..100
    uint8_t source = 0;       // 0 line, 2 mic low, 3 mic high
    uint8_t gain = 50;        // input gain 0..100
    uint8_t monitor = 1;
    uint8_t midiClock = 0;    // MidiClock: off, send, follow
    MidiBinding midiMap[kMidiActions];
    uint16_t latency = 0;     // round-trip latency compensation, output frames (0..kMaxLatencyFrames)
};

class Files {
public:
    static constexpr int kMaxProjects = 99;
    static constexpr int kNameLen = 12;          // a track's name: 11 characters
    static constexpr int kProjectNameLen = 16;   // a project's: 15 (in its folder's name.txt)
    static constexpr int kMaxSamples = 40;       // entries the sample browser lists (internal RAM is tight)
    static constexpr int kSampleNameLen = 28;

    void init(Engine* engine, const Platform* p);
    void setStreamer(Streamer* s) { stream_ = s; }   // long takes: their audio is on the card
    bool cardReady() const;

    bool loadConfig(Config& c);
    bool saveConfig(const Config& c);

    // Projects, numbered folders under Looper/Projects, newest last (the
    // kMaxProjects newest when there are more).
    void rescan();
    int projectCount() const { return count_; }
    int projectNumber(int i) const { return i >= 0 && i < count_ ? numbers_[i] : 0; }
    int current() const { return current_; }   // project number saved or loaded last, 0 = none

    // Start a job; false when one is running or there is no card.
    bool save(bool asNew);          // asNew or no current project: the number after the highest
    bool load(int number);
    bool exportTracks(uint8_t mask);   // bit t = track t; each export is a new numbered folder

    void service();                 // main loop: one step of the running job
    bool busy() const { return kind_ != kNone; }
    bool loading() const { return kind_ == kLoad; }
    float progress() const;         // 0..1 of the running job
    const char* status() const { return status_; }   // the last result or the step running
    // Tracks the running job still reads or writes: a record, clear or edit on
    // one would spoil it (the app refuses them, showing lockReason()).
    uint8_t lockedTracks() const;
    const char* lockReason() const;
    // The last load left tracks out or stopped halfway: the loops are not that
    // project (current() is 0, so F1 saves a new one).
    bool partial() const { return partial_; }

    // Names: the tracks' go with the project (project.bin); a project's in
    // its folder (name.txt), so renaming it rewrites nothing else. Project
    // names are read in the background (service(), one a call): "" until then.
    const char* trackName(int t) const { return t >= 0 && t < kTracks ? trackName_[t] : ""; }
    void setTrackName(int t, const char* s);
    const char* projectName(int i) const { return i >= 0 && i < count_ ? projName_[i] : ""; }
    bool renameProject(int i, const char* name);   // list index i; false: card error

    // The sample loader (S.Load): a WAV from anywhere on the card into an
    // empty track. Folders first, then .wav files, by name.
    void openSampleDir(const char* sub);   // into sub of the current folder; nullptr: up; "": the top
    void listSamples();                    // the current folder, listed again
    const char* sampleDir() const { return sampleDir_; }
    int sampleCount() const { return sampleCount_; }
    const char* sampleName(int i) const { return i >= 0 && i < sampleCount_ ? sampleName_[i] : ""; }
    bool sampleIsDir(int i) const { return i >= 0 && i < sampleCount_ && sampleIsDir_[i]; }
    bool loadSample(int t, int i);   // a job; false: busy, no card, not a WAV, or the track holds a loop
    // Duplicate a long take (part of it on the card) into the next track,
    // empty: a job, the copy read as it plays now; it goes in in time with it.
    bool duplicateLong(int t);

private:
    enum Kind : uint8_t { kNone, kSave, kLoad, kExport, kSample, kDup };
    struct ProjectTrack {
        uint32_t len;        // frames; 0 = empty
        uint32_t phase;      // where its start sits in the bar (anchor mod bar)
        uint8_t mono, stopped, mode, volume, muted, send1, send2, stop;
        uint8_t lenBeats, start, input, after, stretch, pad[3];
    };
    struct ProjectFile {
        char magic[4];       // "LPRJ"
        uint32_t version;    // 3 (1: up to track[]; 2: up to grain[])
        uint32_t bar;        // master bar length, frames (0: no loop)
        uint8_t bpm, beats, metronome, click, threshold, monitor, feedback, master;
        uint8_t ret[kFxSlots], fxType[kFxSlots];
        uint8_t fxKnob[kFxSlots][kFxBank];      // the first bank
        uint8_t perf[kPerfFxCount];
        ProjectTrack track[kTracks];
        // Version 2:
        uint8_t fxKnob2[kFxSlots][kFxBank];     // the second bank (Clouds)
        uint8_t grain[kTracks][kGrainKnobsV2];  // Granular mode's first four knobs
        // Version 3:
        uint8_t stopFade, monitorFx, midiClock, chain[kFxSlots], reserved[11];   // reserved: zero
        // Version 4:
        uint8_t filter[kTracks];                // the mixer's DJ filters
        uint8_t accent, clickSound, clickWhen, countIn;   // the metronome
        uint8_t grain2[kTracks][kGrainKnobs - kGrainKnobsV2];   // Granular's speed and direction
        char trackName[kTracks][kNameLen];
        uint8_t reserved4[16];                  // zero
        // Version 5 (perf[] above: track 1's, for older firmware):
        uint8_t perfTrack[kTracks][kPerfFxCount];   // each track's performance effect knobs
    };
    static constexpr uint32_t kProjectV1Bytes = 136, kProjectV2Bytes = 160, kProjectV3Bytes = 176;
    static_assert(sizeof(ProjectTrack) == 24, "fixed layout");
    static_assert(kProjectV2Bytes == kProjectV1Bytes + kFxSlots * kFxBank + kTracks * kGrainKnobsV2, "fixed layout");
    static_assert(kProjectV3Bytes == kProjectV2Bytes + 16, "fixed layout");
    static constexpr uint32_t kProjectV4Bytes = kProjectV3Bytes + 4 + 4 + 8 + 4 * kNameLen + 16;
    static_assert(sizeof(ProjectFile) == kProjectV4Bytes + kTracks * kPerfFxCount, "fixed layout");
    void readName();                // one project name not read yet, from its name.txt
    bool sampleStep();              // the sample load, a step
    void dupStep();                 // the duplicate, a step

    void snapshot(ProjectFile& pf) const;
    void postSettings(const ProjectFile& pf, bool tracks);
    bool nextFolder(const char* parent, int& number, char* path, size_t n);
    bool openWav(int t);            // start writing track t (save / export)
    bool takeOk() const { return takeOk(track_); }   // the take openWav started on is still there, unchanged
    bool takeOk(int t) const;
    bool writeStep();               // one chunk of the open WAV; false when that track is done
    bool readStep();                // load: one chunk into track_ (duplicate: from src_, as it plays)
    bool commit(const ProjectFile& pf);   // save: the temporary files take the place of the old save
    void tidy();                    // a save that failed: its temporary files go
    void skip(int t, const char* why);    // load: track t stays empty
    void finish(const char* msg);
    void fail(const char* msg);

    Engine* engine_ = nullptr;
    const Platform* p_ = nullptr;
    int numbers_[kMaxProjects] = {};
    int count_ = 0;
    int current_ = 0;

    Kind kind_ = kNone;
    uint8_t phase_ = 0;
    uint8_t todo_ = 0;              // tracks still to write / read
    uint8_t loaded_ = 0;            // load: tracks whose audio is in
    uint8_t long_ = 0;              // load: tracks longer than their RAM (the rest went to the card)
    uint8_t skipped_ = 0;           // load: tracks left empty; export: tracks not written
    const char* why_ = "";          // why the last of them was
    bool partial_ = false;
    uint32_t waitMs_ = 0;           // load: when the wait (stream file, install) began
    uint32_t head_ = 0;             // load: the track's RAM part (a long take's)
    Streamer* stream_ = nullptr;
    int track_ = -1;
    int src_ = -1;                  // duplicate: the track copied (track_ the copy)
    ot::IStorage::Handle h_ = -1;
    uint32_t done_ = 0, total_ = 0, start_ = 0;
    bool mono_ = false;
    bool longTake_ = false;         // save / export: the take being written is a long one
    uint32_t take_ = 0;             //   and its StreamView::take
    uint32_t jobFrames_ = 0, jobDone_ = 0;   // progress over the whole job
    char dir_[48] = {};
    char status_[64] = {};
    int number_ = 0;
    ProjectFile pf_ = {};
    char trackName_[kTracks][kNameLen] = {};
    char projName_[kMaxProjects][kProjectNameLen] = {};
    uint8_t nameRead_[(kMaxProjects + 7) / 8] = {};   // bit i: projName_[i] is read
    char sampleDir_[64] = {};
    char sampleName_[kMaxSamples][kSampleNameLen] = {};
    bool sampleIsDir_[kMaxSamples] = {};
    int sampleCount_ = 0;
    char samplePath_[96] = {};      // the WAV loading
};

}  // namespace looper
