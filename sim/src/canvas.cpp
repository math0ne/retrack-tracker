#include "canvas.h"

#include <algorithm>
#include <cmath>

namespace sim {

#include "../third_party/rom_fonts.inc"

const RomFont* romFont(uint8_t handle) {
    if (handle < 16 || handle > 31) return nullptr;
    return &kRomFonts[handle - 16];
}

int textWidth(uint8_t handle, const char* s) {
    const RomFont* f = romFont(handle);
    if (!f || !s) return 0;
    int w = 0;
    for (; *s; ++s) {
        uint8_t c = uint8_t(*s);
        if (c >= 32 && c < 127) w += f->widths[c - 32];
    }
    return w;
}

void Canvas::setClip(int x0, int y0, int x1, int y1) {
    cx0_ = std::max(0, x0);
    cy0_ = std::max(0, y0);
    cx1_ = std::min(w_, x1);
    cy1_ = std::min(h_, y1);
}

void Canvas::clearRect(uint32_t rgb) {
    const uint32_t v = 0xFF000000u | (rgb & 0xFFFFFF);
    for (int y = cy0_; y < cy1_; ++y)
        std::fill(px_.begin() + size_t(y) * w_ + cx0_, px_.begin() + size_t(y) * w_ + std::max(cx0_, cx1_), v);
}

void Canvas::blendPixel(int x, int y, uint32_t cov) {
    if (x < cx0_ || x >= cx1_ || y < cy0_ || y >= cy1_) return;
    const uint32_t a = (cov * a_ + 127) / 255;
    if (!a) return;
    uint32_t& d = px_[size_t(y) * w_ + x];
    if (a >= 255) {
        d = 0xFF000000u | rgb_;
        return;
    }
    const uint32_t ia = 255 - a;
    const uint32_t r = (((rgb_ >> 16) & 255) * a + ((d >> 16) & 255) * ia) / 255;
    const uint32_t g = (((rgb_ >> 8) & 255) * a + ((d >> 8) & 255) * ia) / 255;
    const uint32_t b = ((rgb_ & 255) * a + (d & 255) * ia) / 255;
    d = 0xFF000000u | (r << 16) | (g << 8) | b;
}

void Canvas::blitRgb565(int x, int y, const uint8_t* src, int w, int h) {
    const int x0 = std::max(x, cx0_), y0 = std::max(y, cy0_);
    const int x1 = std::min(x + w, cx1_), y1 = std::min(y + h, cy1_);
    const uint32_t a = a_;
    if (!a) return;
    for (int py = y0; py < y1; ++py) {
        const uint8_t* row = src + size_t(py - y) * size_t(w) * 2;
        for (int px = x0; px < x1; ++px) {
            const uint16_t c = uint16_t(row[(px - x) * 2] | (row[(px - x) * 2 + 1] << 8));
            uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
            r = ((r << 3) | (r >> 2)) * ((rgb_ >> 16) & 255) / 255;
            g = ((g << 2) | (g >> 4)) * ((rgb_ >> 8) & 255) / 255;
            b = ((b << 3) | (b >> 2)) * (rgb_ & 255) / 255;
            uint32_t& d = px_[size_t(py) * w_ + px];
            if (a < 255) {
                const uint32_t ia = 255 - a;
                r = (r * a + ((d >> 16) & 255) * ia) / 255;
                g = (g * a + ((d >> 8) & 255) * ia) / 255;
                b = (b * a + (d & 255) * ia) / 255;
            }
            d = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

void Canvas::fillRect(int x0, int y0, int x1, int y1) {
    x0 = std::max(x0, cx0_);
    y0 = std::max(y0, cy0_);
    x1 = std::min(x1, cx1_);
    y1 = std::min(y1, cy1_);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) blendPixel(x, y, 255);
}

namespace {
inline uint32_t coverage(float dist, float r) {
    // 1-px antialiased edge around radius r.
    float c = r - dist + 0.5f;
    if (c <= 0) return 0;
    if (c >= 1) return 255;
    return uint32_t(c * 255.0f);
}
}  // namespace

void Canvas::fillRoundRect(float x0, float y0, float x1, float y1, float r) {
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    const int ix0 = int(std::floor(x0 - r)), iy0 = int(std::floor(y0 - r));
    const int ix1 = int(std::ceil(x1 + r)) + 1, iy1 = int(std::ceil(y1 + r)) + 1;
    for (int y = std::max(iy0, cy0_); y < std::min(iy1, cy1_); ++y) {
        const float py = float(y) + 0.5f;
        const float dy = py < y0 ? y0 - py : (py > y1 ? py - y1 : 0.0f);
        for (int x = std::max(ix0, cx0_); x < std::min(ix1, cx1_); ++x) {
            const float px = float(x) + 0.5f;
            const float dx = px < x0 ? x0 - px : (px > x1 ? px - x1 : 0.0f);
            const uint32_t c = coverage(std::sqrt(dx * dx + dy * dy), r);
            if (c) blendPixel(x, y, c);
        }
    }
}

void Canvas::fillCircle(float cx, float cy, float r) { fillRoundRect(cx, cy, cx, cy, r); }

void Canvas::fillCapsule(float x0, float y0, float x1, float y1, float r) {
    const float vx = x1 - x0, vy = y1 - y0;
    const float len2 = vx * vx + vy * vy;
    const int ix0 = int(std::floor(std::min(x0, x1) - r)), iy0 = int(std::floor(std::min(y0, y1) - r));
    const int ix1 = int(std::ceil(std::max(x0, x1) + r)) + 1, iy1 = int(std::ceil(std::max(y0, y1) + r)) + 1;
    for (int y = std::max(iy0, cy0_); y < std::min(iy1, cy1_); ++y) {
        const float py = float(y) + 0.5f;
        for (int x = std::max(ix0, cx0_); x < std::min(ix1, cx1_); ++x) {
            const float px = float(x) + 0.5f;
            float t = len2 > 0 ? ((px - x0) * vx + (py - y0) * vy) / len2 : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            const float dx = px - (x0 + t * vx), dy = py - (y0 + t * vy);
            const uint32_t c = coverage(std::sqrt(dx * dx + dy * dy), r);
            if (c) blendPixel(x, y, c);
        }
    }
}

void Canvas::glyph(int x, int y, uint8_t handle, uint8_t ch) {
    const RomFont* f = romFont(handle);
    if (!f || ch < 32 || ch > 126) return;
    const int w = f->widths[ch - 32];
    const uint8_t* g = f->px + f->offsets[ch - 32];
    for (int j = 0; j < f->height; ++j)
        for (int i = 0; i < w; ++i)
            if (g[j * w + i]) blendPixel(x + i, y + j, g[j * w + i]);
}

void Canvas::blitL4(int x, int y, const uint8_t* src, int stride, int srcW, int srcH, int drawW, int drawH,
                    const int32_t t[6]) {
    const int y0 = std::max(y, cy0_), y1 = std::min(y + drawH, cy1_);
    const int x0 = std::max(x, cx0_), x1 = std::min(x + drawW, cx1_);
    for (int py = y0; py < y1; ++py) {
        const int64_t ly = py - y;
        for (int px = x0; px < x1; ++px) {
            const int64_t lx = px - x;
            // Pixel centre (lx + 0.5, ly + 0.5) through the 8.8 transform.
            const int64_t uf = t[0] * lx + t[1] * ly + t[2] + (t[0] + t[1]) / 2;
            const int64_t vf = t[3] * lx + t[4] * ly + t[5] + (t[3] + t[4]) / 2;
            const int64_t u = uf >> 8, v = vf >> 8;   // floor
            if (u < 0 || v < 0 || u >= srcW || v >= srcH) continue;
            const uint8_t b = src[v * stride + (u >> 1)];
            const uint32_t n = (u & 1) ? (b & 15u) : (b >> 4);
            if (n) blendPixel(px, py, n * 17u);
        }
    }
}

void Canvas::text(int x, int y, uint8_t handle, const char* s) {
    const RomFont* f = romFont(handle);
    if (!f || !s) return;
    for (; *s; ++s) {
        const uint8_t c = uint8_t(*s);
        if (c < 32 || c > 126) continue;
        glyph(x, y, handle, c);
        x += f->widths[c - 32];
    }
}

}  // namespace sim
