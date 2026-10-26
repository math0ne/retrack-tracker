// Simulator INvStore: the config EEPROM app area kept in a host file.
#pragma once

#include <string>
#include <vector>

#include "hal_api.h"

class SimNvStore final : public ot::INvStore {
public:
    static constexpr uint32_t kSize = 4096 - 10;   // app area of the 4 KiB EEPROM (spec 02 §9)
    // Loads `path` if it exists (missing file = erased EEPROM, all 0xFF).
    bool open(const std::string& path);
    uint32_t size() const override { return kSize; }
    bool read(uint32_t offset, void* dst, uint32_t len) override;
    bool write(uint32_t offset, const void* src, uint32_t len) override;

private:
    std::string path_;
    std::vector<uint8_t> data_;
};
