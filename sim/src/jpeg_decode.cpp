#include "jpeg_decode.h"

#include <cstring>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "../third_party/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace sim {

bool decodeJpegRgb(const uint8_t* data, size_t n, std::vector<uint8_t>& rgb, int& w, int& h) {
    w = h = 0;
    if (!data || n == 0 || n > 0x7FFFFFFF) return false;
    int comp = 0;
    stbi_uc* px = stbi_load_from_memory(data, int(n), &w, &h, &comp, 3);
    if (!px) return false;
    rgb.assign(px, px + size_t(w) * size_t(h) * 3);
    stbi_image_free(px);
    return true;
}

}  // namespace sim
