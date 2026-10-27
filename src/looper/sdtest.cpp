#include "looper/sdtest.h"

#include <cstdio>
#include <cstring>

namespace looper {

using ot::IStorage;

namespace {
constexpr char kPath[] = "/Looper/sdtest.bin";
constexpr char kReport[] = "/Looper/sdtest.txt";

struct PhaseDef {
    const char* name;
    uint32_t bytes;
    bool write, dma;
};
constexpr PhaseDef kPhase[SdTest::kPhases] = {
    {"Allocate", 256 * 1024, true, false},
    {"Overwrite", 32 * 1024, true, false},       {"Read", 32 * 1024, false, false},
    {"Overwrite", 64 * 1024, true, false},       {"Read", 64 * 1024, false, false},
    {"Overwrite", 256 * 1024, true, false},      {"Read", 256 * 1024, false, false},
    {"Overwrite DMA", 256 * 1024, true, true},   {"Read DMA", 256 * 1024, false, true},
};
}  // namespace

constexpr uint32_t SdTest::kBinUs[SdTest::kBins];

uint32_t SdTest::cmdBytes(int phase) { return kPhase[phase < 0 ? 0 : phase].bytes; }

const char* SdTest::phaseName(int phase) { return kPhase[phase < 0 ? 0 : phase].name; }

bool SdTest::ready() const {
    return p_ && p_->storage && (!p_->storageReady || p_->storageReady());
}

bool SdTest::stats(Platform::SdStats& s) const {
    s = {};
    return p_->sdStats && p_->sdStats(s);
}

bool SdTest::start(uint8_t* buf) {
    if (running() || !buf || !ready()) return false;
    buf_ = reinterpret_cast<uint32_t*>(buf);
    for (int i = 0; i < kPhases; ++i) {
        res_[i] = Result{};
        res_[i].cmdBytes = kPhase[i].bytes;
        res_[i].write = kPhase[i].write;
        res_[i].dma = kPhase[i].dma;
    }
    done_ = 0;
    h_ = -1;
    phase_ = 0;
    ran_ = false;
    dmaFailed_ = false;
    dmaNote_[0] = 0;
    stats(s0_);
    p_->storage->mkdir("/Looper");
    std::snprintf(status_, sizeof status_, "Writing the 16 MB test file...");
    return true;
}

void SdTest::stop() {
    if (!running()) return;
    if (p_->sdDirectDma) p_->sdDirectDma(false);
    if (h_ >= 0) p_->storage->close(h_);
    h_ = -1;
    if (ready()) p_->storage->remove(kPath);
    phase_ = -1;
    std::snprintf(status_, sizeof status_, "Stopped");
}

float SdTest::progress() const {
    if (!running()) return ran_ ? 1.0f : 0.0f;
    return (float(phase_) + float(done_) / float(kFileBytes)) / float(kPhases);
}

uint32_t SdTest::verifyErrors() const {
    uint32_t n = 0;
    for (const Result& r : res_) n += r.errors;
    return n;
}

// A word pattern that differs per word and per pass, so a read checks that it
// got the right sector of the latest write.
void SdTest::fill(uint32_t word0, uint32_t seed) {
    for (uint32_t i = 0; i < kBufBytes / 4; ++i) buf_[i] = (word0 + i) * 2654435761u ^ seed;
}

uint32_t SdTest::check(uint32_t word0, uint32_t seed, uint32_t n) const {
    uint32_t bad = 0;
    for (uint32_t i = 0; i < n / 4; ++i)
        if (buf_[i] != ((word0 + i) * 2654435761u ^ seed)) ++bad;
    return bad;
}

void SdTest::account(Result& r, uint32_t us, uint32_t bytes) {
    r.bytes += bytes;
    if (bytes) ++r.cmds;
    r.totalUs += us;
    if (us > r.maxUs) r.maxUs = us;
    for (int b = 0; b < kBins; ++b)
        if (us > kBinUs[b]) ++r.over[b];
}

void SdTest::service() {
    if (!running()) return;
    if (!ready()) {
        fail("The SD card went away");
        return;
    }
    IStorage& fs = *p_->storage;
    Result& r = res_[phase_];
    // The pass the file holds now: a read checks the overwrite just before it.
    const uint32_t seed = r.write ? uint32_t(phase_) : uint32_t(phase_ - 1);
    if (h_ < 0) {
        h_ = fs.open(kPath, phase_ == 0 ? IStorage::kWrite : (r.write ? IStorage::kReadWrite : IStorage::kRead));
        if (h_ < 0 || (phase_ > 0 && !fs.seek(h_, 0))) {
            fail("Could not open the test file");
            return;
        }
        done_ = 0;
    }
    const uint32_t n = r.cmdBytes;
    const uint32_t word0 = done_ / 4;
    Platform::SdStats a = {}, b = {};
    if (r.dma) stats(a);
    // Direct DMA only around the test's own transfer (FatFs's sectors are in SRAM).
    const bool dma = r.dma && !dmaFailed_ && p_->sdDirectDma && p_->sdDirectDma(true);
    int32_t got;
    if (r.write) {
        fill(word0, seed);
        const uint32_t t0 = p_->clock->micros();
        got = fs.write(h_, buf_, n);
        account(r, p_->clock->micros() - t0, n);
    } else {
        const uint32_t t0 = p_->clock->micros();
        got = fs.read(h_, buf_, n);
        account(r, p_->clock->micros() - t0, n);
    }
    if (dma) p_->sdDirectDma(false);   // off again (a failure already turned it off)
    if (r.dma && stats(b)) {
        r.direct += b.direct - a.direct;
        if (b.directFails != a.directFails) dmaFailed_ = true;
    }
    if (got != int32_t(n)) {
        fail(r.write ? (phase_ == 0 ? "Write failed: is there 16 MB free?" : "Write failed") : "Read failed");
        return;
    }
    if (!r.write) r.errors += check(word0, seed, n);
    done_ += n;
    if (done_ < kFileBytes) return;
    // The phase's last step: closing flushes what the card still holds, so it
    // counts toward the time (as part of the last command).
    const uint32_t t0 = p_->clock->micros();
    fs.close(h_);
    h_ = -1;
    if (r.write) account(r, p_->clock->micros() - t0, 0);
    r.done = true;
    if (++phase_ == kPhases) {
        finish();
        return;
    }
    std::snprintf(status_, sizeof status_, "%s, %lu KB commands...", phaseName(phase_),
                  static_cast<unsigned long>(cmdBytes(phase_) / 1024));
}

void SdTest::line(int phase, char* buf, size_t n) const {
    const Result& r = res_[phase];
    if (!r.cmds) {
        std::snprintf(buf, n, "%s %lu KB", phaseName(phase), static_cast<unsigned long>(r.cmdBytes / 1024));
        return;
    }
    const double s = double(r.totalUs) / 1e6;
    const double mbs = s > 0.0 ? double(r.bytes) / (1024.0 * 1024.0) / s : 0.0;
    int k = std::snprintf(buf, n, "%-13s %3lu KB  %5.1f MB/s  avg %5.1f ms  worst %4lu ms  >10:%lu >50:%lu >100:%lu >250:%lu",
                          phaseName(phase), static_cast<unsigned long>(r.cmdBytes / 1024), mbs,
                          double(r.totalUs) / 1000.0 / double(r.cmds), static_cast<unsigned long>(r.maxUs / 1000),
                          static_cast<unsigned long>(r.over[0]), static_cast<unsigned long>(r.over[1]),
                          static_cast<unsigned long>(r.over[2]), static_cast<unsigned long>(r.over[3]));
    if (k > 0 && size_t(k) < n && !r.write)
        k += std::snprintf(buf + k, n - size_t(k), "  bad words %lu", static_cast<unsigned long>(r.errors));
    if (k > 0 && size_t(k) < n && r.dma)
        std::snprintf(buf + k, n - size_t(k), "  direct %lu", static_cast<unsigned long>(r.direct));
}

void SdTest::finish() {
    phase_ = -1;
    ran_ = true;
    if (p_->sdDirectDma) p_->sdDirectDma(false);
    p_->storage->remove(kPath);
    uint32_t worstW = 0;
    for (int i = 1; i < kPhases; ++i)
        if (res_[i].write && !res_[i].dma && res_[i].maxUs > worstW) worstW = res_[i].maxUs;
    const uint32_t bad = verifyErrors();
    if (bad)
        std::snprintf(status_, sizeof status_, "Done: %lu words read back wrong", static_cast<unsigned long>(bad));
    else
        std::snprintf(status_, sizeof status_, "Done: worst overwrite %lu ms, read back OK",
                      static_cast<unsigned long>(worstW / 1000));
    // The direct DMA verdict, with what the driver saw over the whole test.
    Platform::SdStats s1 = {};
    const Result &dw = res_[kPhases - 2], &dr = res_[kPhases - 1];
    if (!p_->sdDirectDma) {
        std::snprintf(dmaNote_, sizeof dmaNote_, "Direct DMA: not on this platform (the DMA passes used the normal path)");
    } else {
        const char* v = dmaFailed_ ? "FAILED, turned off" : (dr.errors ? "data came back WRONG"
                                                                 : (dw.direct && dr.direct ? "works, data checked" : "not used"));
        if (stats(s1))
            std::snprintf(dmaNote_, sizeof dmaNote_,
                          "Direct DMA: %s. Driver: %lu direct, %lu failed, %lu bounced, %lu retries, %lu slow-downs, %lu MHz",
                          v, static_cast<unsigned long>(s1.direct - s0_.direct),
                          static_cast<unsigned long>(s1.directFails - s0_.directFails),
                          static_cast<unsigned long>(s1.bounced - s0_.bounced),
                          static_cast<unsigned long>(s1.retries - s0_.retries),
                          static_cast<unsigned long>(s1.degrades - s0_.degrades),
                          static_cast<unsigned long>(s1.clockHz / 1000000));
        else
            std::snprintf(dmaNote_, sizeof dmaNote_, "Direct DMA: %s", v);
    }
    report();
}

void SdTest::fail(const char* msg) {
    if (p_->sdDirectDma) p_->sdDirectDma(false);
    if (h_ >= 0) p_->storage->close(h_);
    h_ = -1;
    if (ready()) p_->storage->remove(kPath);
    phase_ = -1;
    std::snprintf(status_, sizeof status_, "%s", msg);
}

void SdTest::report() {
    // The serial log and Looper/sdtest.txt, so the numbers can be passed on.
    char ln[200];
    const IStorage::Handle h = p_->storage->open(kReport, IStorage::kWrite);
    auto put = [&](const char* s) {
        if (p_->log) p_->log(s);
        if (h >= 0) {
            p_->storage->write(h, s, uint32_t(std::strlen(s)));
            p_->storage->write(h, "\r\n", 2);
        }
    };
    put("sdtest: 16 MB file; overwrite = in place (no FAT work), read = checked; DMA = direct to SDRAM");
    for (int i = 0; i < kPhases; ++i) {
        char body[180];
        line(i, body, sizeof body);
        std::snprintf(ln, sizeof ln, "sdtest: %s", body);
        put(ln);
    }
    std::snprintf(ln, sizeof ln, "sdtest: %s", status_);
    put(ln);
    std::snprintf(ln, sizeof ln, "sdtest: %s", dmaNote_);
    put(ln);
    if (h >= 0) p_->storage->close(h);
}

}  // namespace looper
