// Software CRC-32/ISO-HDLC, spec 02 §2. Table generated at compile time.
#include "formats/crc32.h"

namespace ot {

namespace {
struct Table {
    uint32_t t[256];
    constexpr Table() : t() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
    }
};
constexpr Table kTable;
Crc32Hook g_hook = nullptr;
}  // namespace

void setCrc32Hook(Crc32Hook hook) { g_hook = hook; }

uint32_t crc32Update(uint32_t state, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) state = kTable.t[(state ^ data[i]) & 0xFF] ^ (state >> 8);
    return state;
}

uint32_t crc32Software(const uint8_t* data, size_t len) {
    return crc32Final(crc32Update(crc32Init(), data, len));
}

uint32_t crc32(const uint8_t* data, size_t len) {
    return g_hook ? g_hook(data, len) : crc32Software(data, len);
}

}  // namespace ot
