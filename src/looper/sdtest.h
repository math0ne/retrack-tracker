// The SD card test (Config > F3 SD TEST): how fast and, above all, how
// steadily the card writes and reads in the command sizes that streaming
// loops from the card would use (docs/SD_STREAMING.md, "First step").
//
// A 16 MB test file is written once in 256 KB commands (that pass also
// allocates its clusters), then for 32, 64 and 256 KB commands it is
// overwritten in place (no FAT work: what streaming does) and read back with
// every word checked. A last overwrite + read at 256 KB tries the experimental
// direct DMA to / from SDRAM (Platform::sdDirectDma) instead of the SRAM
// bounce buffer; the driver turns that path off by itself if it fails, and the
// read-back catches anything it gets wrong silently. Each command is timed; the
// worst ones are what the streaming buffers have to ride out. Like the File
// page's jobs, service() does one command per main-loop pass. The results go
// to the screen, the serial log and Looper/sdtest.txt; the test file is
// deleted afterwards.
#pragma once

#include <cstddef>
#include <cstdint>

#include "hal_api.h"
#include "looper/platform.h"

namespace looper {

class SdTest {
public:
    static constexpr uint32_t kBufBytes = 256 * 1024;   // the largest command
    static constexpr uint32_t kFileBytes = 16u << 20;
    static constexpr int kPhases = 9;   // allocate; overwrite + read at 32, 64, 256 KB; the same at 256 KB with DMA
    static constexpr int kBins = 4;     // commands over 10, 50, 100, 250 ms
    static constexpr uint32_t kBinUs[kBins] = {10000, 50000, 100000, 250000};

    struct Result {
        uint32_t cmdBytes = 0;
        bool write = true, dma = false;
        uint32_t bytes = 0, cmds = 0;
        uint64_t totalUs = 0;
        uint32_t maxUs = 0;
        uint32_t over[kBins] = {};
        uint32_t errors = 0;    // read: words that came back wrong
        uint32_t direct = 0;    // DMA passes: chunks the driver sent straight to / from SDRAM
        bool done = false;
    };

    void init(const Platform* p) { p_ = p; }
    // buf: kBufBytes of SDRAM, the test's to use until it ends. False without a card.
    bool start(uint8_t* buf);
    void stop();      // abandon it (the test file is removed)
    void service();   // main loop: one command of the running test
    bool running() const { return phase_ >= 0; }
    bool ran() const { return ran_; }   // a test has finished (its results stand)
    int phase() const { return phase_; }
    float progress() const;
    const Result& result(int phase) const { return res_[phase]; }
    uint32_t verifyErrors() const;
    const char* status() const { return status_; }
    // The direct-DMA verdict and the driver's counts, one line ("" before a run).
    const char* dmaNote() const { return dmaNote_; }
    // One line of results ("Overwrite  64 KB  6.1 MB/s  avg 10.4 ms  worst 38 ms ...").
    void line(int phase, char* buf, size_t n) const;
    static const char* phaseName(int phase);
    static uint32_t cmdBytes(int phase);

private:
    void fill(uint32_t word0, uint32_t seed);
    uint32_t check(uint32_t word0, uint32_t seed, uint32_t n) const;
    void account(Result& r, uint32_t us, uint32_t bytes);
    bool stats(Platform::SdStats& s) const;
    void finish();
    void fail(const char* msg);
    void report();
    bool ready() const;

    const Platform* p_ = nullptr;
    uint32_t* buf_ = nullptr;
    int phase_ = -1;
    bool ran_ = false;
    ot::IStorage::Handle h_ = -1;
    uint32_t done_ = 0;   // bytes of the file done in this phase
    Result res_[kPhases];
    Platform::SdStats s0_ = {};   // the driver's counts when the test started
    bool dmaFailed_ = false;      // the direct path turned itself off during the test
    char status_[64] = {};
    char dmaNote_[192] = {};
};

}  // namespace looper
