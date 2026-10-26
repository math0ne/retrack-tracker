// Minimal PNG writer (RGB8, zlib "stored" deflate blocks, no compression).
// Written for the simulator; no third-party code.
#pragma once
#include <cstdint>

namespace sim {
// argb: w*h pixels 0xAARRGGBB, row-major.
bool writePng(const char* path, const uint32_t* argb, int w, int h);
}
