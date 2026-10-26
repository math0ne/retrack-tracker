// Simulator INvStore (file-backed stand-in for the FlexRAM EEPROM app area).
#include "sim_nvstore.h"

#include <cstdio>
#include <cstring>

bool SimNvStore::open(const std::string& path) {
    path_ = path;
    data_.assign(kSize, 0xFF);
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        const size_t n = std::fread(data_.data(), 1, data_.size(), f);
        (void)n;
        std::fclose(f);
    }
    return true;
}

bool SimNvStore::read(uint32_t offset, void* dst, uint32_t len) {
    if (data_.empty() || offset > kSize || len > kSize - offset) return false;
    std::memcpy(dst, data_.data() + offset, len);
    return true;
}

bool SimNvStore::write(uint32_t offset, const void* src, uint32_t len) {
    if (data_.empty() || offset > kSize || len > kSize - offset) return false;
    if (std::memcmp(data_.data() + offset, src, len) == 0) return true;
    std::memcpy(data_.data() + offset, src, len);
    FILE* f = std::fopen(path_.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data_.data(), 1, data_.size(), f) == data_.size();
    std::fclose(f);
    return ok;
}
