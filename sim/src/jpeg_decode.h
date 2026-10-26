// OpenTracker simulator -- host JPEG decode (stb_image, third_party/stb_image.h,
// public domain / MIT). Stands in for the FT81x CMD_LOADIMAGE decoder.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sim {

// Decodes a JPEG in memory to 8-bit RGB (3 B/px, row-major). False on error.
bool decodeJpegRgb(const uint8_t* data, size_t n, std::vector<uint8_t>& rgb, int& w, int& h);

}  // namespace sim
