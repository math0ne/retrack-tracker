// OpenTracker -- IStorage on the SD card via FatFs (spec 01 §6).
#pragma once

#include <stdint.h>
#include "hal_api.h"

namespace ot {
namespace hal {

class SdStorage final : public IStorage {
public:
    // Card-detect watcher (spec 01 §6: 500 ms debounce, insert -> init+mount,
    // removal -> unmount). Call from the main loop.
    void service();
    bool mount();
    void unmount();
    bool mounted() const { return mounted_; }
    // USB storage mode (spec 01 §6.1): release the card, stop all SDHC access.
    void setStorageMode(bool on);

    // Throughput self-measurement (selftest): writes `totalBytes` to a temp file in
    // `bufLen` chunks from `buf`, reads it back (checking chunk tags), deletes it.
    struct Bench { bool ok = false; uint32_t bytes = 0, writeUs = 0, readUs = 0, verifyErrors = 0; };
    bool benchmark(uint8_t* buf, uint32_t bufLen, uint32_t totalBytes, Bench& r);

    Handle open(const char* path, OpenMode mode) override;
    int32_t read(Handle h, void* dst, uint32_t len) override;
    int32_t write(Handle h, const void* src, uint32_t len) override;
    bool seek(Handle h, uint32_t pos) override;
    int32_t size(Handle h) override;
    void close(Handle h) override;
    bool exists(const char* path) override;
    bool mkdir(const char* path) override;
    bool remove(const char* path) override;
    bool rename(const char* from, const char* to) override;
    int32_t listDir(const char* path, DirFn fn, void* ctx) override;

private:
    bool mounted_ = false;
    bool storageMode_ = false;
    bool lastPresent_ = false;
    uint32_t changeMs_ = 0;
    bool pendingPresent_ = false;
};

}  // namespace hal
}  // namespace ot
