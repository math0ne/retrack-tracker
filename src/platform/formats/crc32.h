// CRC-32/ISO-HDLC (zlib crc32), spec 02 §2.
// width 32, poly 0x04C11DB7 (reflected 0xEDB88320), init 0xFFFFFFFF,
// refin/refout true, xorout 0xFFFFFFFF, check("123456789") = 0xCBF43926.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ot {

// Optional accelerator (e.g. the MK66 CRC unit). It must compute the full
// one-shot CRC with the parameters above. nullptr restores the software path.
using Crc32Hook = uint32_t (*)(const uint8_t* data, size_t len);
void setCrc32Hook(Crc32Hook hook);

// One-shot CRC (uses the hook when installed).
uint32_t crc32(const uint8_t* data, size_t len);

// Software incremental form: crc = crc32Update(crc32Init(), ...); crc32Final(crc).
inline uint32_t crc32Init() { return 0xFFFFFFFFu; }
uint32_t crc32Update(uint32_t state, const uint8_t* data, size_t len);
inline uint32_t crc32Final(uint32_t state) { return state ^ 0xFFFFFFFFu; }

// Software-only one-shot (ignores the hook).
uint32_t crc32Software(const uint8_t* data, size_t len);

}  // namespace ot
