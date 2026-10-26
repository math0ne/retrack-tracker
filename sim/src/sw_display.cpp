#include "sw_display.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "jpeg_decode.h"
#include "ui/fonts/fonts.h"

namespace sim {

SwDisplay::SwDisplay() : back_(kW, kH), ramg_(kRamGSize, 0), front_(size_t(kW) * kH, 0xFF000000u) { back_.resetClip(); }

void SwDisplay::warnOnce(int key, const char* msg, int v) {
    if (key < 0 || key >= 64 || warned_[key]) return;
    warned_[key] = true;
    std::fprintf(stderr, "sim display: %s (%d) -- ignored (logged once)\n", msg, v);
}

void SwDisplay::dlStart() {
    // Fresh display list: default graphics context (FT81x reset values).
    clearRgb_ = 0;
    rgb_ = 0xFFFFFF;
    a_ = 255;
    lineW_ = 16;
    pointS_ = 16;
    scX_ = scY_ = 0;
    scW_ = scH_ = 2048;
    translateX_ = 0;
    static const int32_t kIdentity[6] = {256, 0, 0, 0, 256, 0};
    std::memcpy(xform_, kIdentity, sizeof(xform_));
    ctxDepth_ = 0;
    prim_ = 0;
    nvert_ = 0;
    back_.resetClip();
    back_.clearRect(0);
    applyColor();
    if (fontsBound_) setupFontHandles();   // as the FT81x driver after CMD_DLSTART
}

namespace {
uint32_t rd32(const std::vector<uint8_t>& m, uint32_t a) {
    return uint32_t(m[a]) | (uint32_t(m[a + 1]) << 8) | (uint32_t(m[a + 2]) << 16) | (uint32_t(m[a + 3]) << 24);
}
}  // namespace

bool SwDisplay::bindFontSet(int set) {
    if (!fontsUploaded_) fontsUploaded_ = writeRamG(ot::fontdata::kRamGBase, ot::fontdata::kBlob, ot::fontdata::kBlobSize);
    fontSet_ = set == 1 ? 1 : 0;
    fontsBound_ = fontsUploaded_;
    return fontsBound_;
}

void SwDisplay::setupFontHandles() {
    // BITMAP_SOURCE/LAYOUT/SIZE + CMD_SETFONT per handle, read back from the
    // metric blocks in RAM_G (what the coprocessor sees on the device).
    for (int i = 0; i < kFontHandleCount; ++i) {
        const ot::fontdata::FontInfo& f = ot::fontdata::info(fontSet_, i);
        const uint32_t m = ot::fontdata::kRamGBase + f.metric;
        Handle& h = handles_[kFontHandleBase + i];
        if (rd32(ramg_, m + 128) != 2) warnOnce(8, "custom font is not L4", int(rd32(ramg_, m + 128)));
        h.valid = true;
        h.metric = m;
        h.stride = uint16_t(rd32(ramg_, m + 132));
        h.sizeW = uint16_t(rd32(ramg_, m + 136));
        h.layoutH = h.sizeH = uint16_t(rd32(ramg_, m + 140));
        h.source = rd32(ramg_, m + 144);
    }
    const ot::fontdata::IconSheet& ic = ot::fontdata::kIcons;
    Handle& h = handles_[kIconHandle];
    h.valid = true;
    h.metric = 0;
    h.source = ot::fontdata::kRamGBase + ic.offset;
    h.stride = ic.stride;
    h.sizeW = ic.w;
    h.layoutH = h.sizeH = ic.h;
}

void SwDisplay::bitmapTransform(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f) {
    xform_[0] = a; xform_[1] = b; xform_[2] = c;
    xform_[3] = d; xform_[4] = e; xform_[5] = f;
}

void SwDisplay::bitmapSize(uint8_t handle, uint16_t w, uint16_t h) {
    if (handle >= 16 || !handles_[handle].valid) {
        warnOnce(9, "bitmapSize on a handle without a bitmap", handle);
        return;
    }
    handles_[handle].sizeW = w;
    handles_[handle].sizeH = h;
}

void SwDisplay::customGlyph(int x, int y, uint8_t handle, uint8_t cell) {
    const Handle& h = handles_[handle];
    const uint32_t cellBytes = uint32_t(h.stride) * h.layoutH;
    const uint32_t addr = h.source + cell * cellBytes;
    if (addr + cellBytes > kRamGSize) return;
    back_.blitL4(x, y, ramg_.data() + addr, h.stride, h.stride * 2, h.layoutH, h.sizeW, h.sizeH, xform_);
}

void SwDisplay::applyClip() {
    back_.setClip(scX_, scY_, int(scX_) + scW_, int(scY_) + scH_);
}
void SwDisplay::applyColor() { back_.setColor(rgb_, a_); }

void SwDisplay::clearColorRGB(uint8_t r, uint8_t g, uint8_t b) { clearRgb_ = (uint32_t(r) << 16) | (g << 8) | b; }

void SwDisplay::clear(bool color, bool, bool) {
    if (!color) return;
    applyClip();
    back_.clearRect(clearRgb_);
}

void SwDisplay::colorRGB(uint8_t r, uint8_t g, uint8_t b) {
    rgb_ = (uint32_t(r) << 16) | (g << 8) | b;
    applyColor();
}
void SwDisplay::colorA(uint8_t a) {
    a_ = a;
    applyColor();
}
void SwDisplay::lineWidth(uint16_t w) { lineW_ = w; }
void SwDisplay::pointSize(uint16_t s) { pointS_ = s; }
void SwDisplay::scissorXY(uint16_t x, uint16_t y) { scX_ = x; scY_ = y; }
void SwDisplay::scissorSize(uint16_t w, uint16_t h) { scW_ = w; scH_ = h; }

void SwDisplay::begin(Prim prim) {
    if (prim < kBitmaps || prim > kRects) warnOnce(1, "unsupported BEGIN primitive", prim);
    prim_ = prim;
    nvert_ = 0;
}
void SwDisplay::end() {
    prim_ = 0;
    nvert_ = 0;
}

void SwDisplay::vertex2f(int32_t x16, int32_t y16) {
    vertex(float(x16 + translateX_) / 16.0f, float(y16) / 16.0f, 0, 0, false);
}
void SwDisplay::vertex2ii(uint16_t x, uint16_t y, uint8_t handle, uint8_t cell) {
    vertex(float(x) + float(translateX_) / 16.0f, float(y), handle, cell, true);
}

void SwDisplay::saveContext() {
    if (ctxDepth_ >= 4) {
        warnOnce(6, "SAVE_CONTEXT stack overflow", ctxDepth_);
        return;
    }
    Context& c = ctxStack_[ctxDepth_++];
    c = {clearRgb_, rgb_, a_, lineW_, pointS_, scX_, scY_, scW_, scH_, translateX_, {}};
    std::memcpy(c.xform, xform_, sizeof(xform_));
}

void SwDisplay::restoreContext() {
    if (ctxDepth_ <= 0) {
        warnOnce(7, "RESTORE_CONTEXT without SAVE_CONTEXT", 0);
        return;
    }
    const Context& c = ctxStack_[--ctxDepth_];
    clearRgb_ = c.clearRgb;
    rgb_ = c.rgb;
    a_ = c.a;
    lineW_ = c.lineW;
    pointS_ = c.pointS;
    scX_ = c.scX;
    scY_ = c.scY;
    scW_ = c.scW;
    scH_ = c.scH;
    translateX_ = c.translateX;
    std::memcpy(xform_, c.xform, sizeof(xform_));
    applyColor();
}

void SwDisplay::gradient(int16_t x0, int16_t y0, uint32_t rgb0, int16_t x1, int16_t y1, uint32_t rgb1) {
    // Colour = rgb0 + t * (rgb1 - rgb0), t = projection of the pixel centre
    // on (x0, y0) -> (x1, y1), clamped to 0..1; fills scissor ∩ screen.
    const int cx0 = std::max<int>(scX_, 0), cy0 = std::max<int>(scY_, 0);
    const int cx1 = std::min<int>(scX_ + scW_, kW), cy1 = std::min<int>(scY_ + scH_, kH);
    const float dx = float(x1 - x0), dy = float(y1 - y0);
    const float len2 = dx * dx + dy * dy;
    const int r0 = int((rgb0 >> 16) & 255), g0 = int((rgb0 >> 8) & 255), b0 = int(rgb0 & 255);
    const int r1 = int((rgb1 >> 16) & 255), g1 = int((rgb1 >> 8) & 255), b1 = int(rgb1 & 255);
    uint32_t* px = back_.pixels();
    for (int y = cy0; y < cy1; ++y) {
        for (int x = cx0; x < cx1; ++x) {
            float t = len2 > 0 ? ((float(x) - float(x0)) * dx + (float(y) - float(y0)) * dy) / len2 : 1.0f;
            t = t < 0 ? 0 : (t > 1 ? 1 : t);
            const uint32_t r = uint32_t(float(r0) + t * float(r1 - r0) + 0.5f);
            const uint32_t g = uint32_t(float(g0) + t * float(g1 - g0) + 0.5f);
            const uint32_t b = uint32_t(float(b0) + t * float(b1 - b0) + 0.5f);
            px[size_t(y) * kW + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

void SwDisplay::edgeSegment(float x0, float y0, float x1, float y1) {
    // Fill from the segment to the screen edge (R: right, L: left, A: above, B: below).
    const bool vertical = prim_ == kEdgeStripR || prim_ == kEdgeStripL;
    if (vertical) {
        if (y1 < y0) { std::swap(x0, x1); std::swap(y0, y1); }
        for (int y = int(y0 + 0.5f); y < int(y1 + 0.5f); ++y) {
            const float t = y1 > y0 ? (float(y) + 0.5f - y0) / (y1 - y0) : 0.0f;
            const int x = int(x0 + t * (x1 - x0) + 0.5f);
            if (prim_ == kEdgeStripR) back_.fillRect(x, y, kW, y + 1);
            else back_.fillRect(0, y, x, y + 1);
        }
    } else {
        if (x1 < x0) { std::swap(x0, x1); std::swap(y0, y1); }
        for (int x = int(x0 + 0.5f); x < int(x1 + 0.5f); ++x) {
            const float t = x1 > x0 ? (float(x) + 0.5f - x0) / (x1 - x0) : 0.0f;
            const int y = int(y0 + t * (y1 - y0) + 0.5f);
            if (prim_ == kEdgeStripB) back_.fillRect(x, y, x + 1, kH);
            else back_.fillRect(x, 0, x + 1, y);
        }
    }
}

void SwDisplay::vertex(float x, float y, uint8_t handle, uint8_t cell, bool ii) {
    applyClip();
    const float lr = float(lineW_) / 16.0f;
    switch (prim_) {
        case kBitmaps:
            if (handle < 16 && handles_[handle].valid) customGlyph(int(x), int(y), handle, cell);
            else if (romFont(handle)) back_.glyph(int(x), int(y), handle, cell);
            else warnOnce(32 + (handle & 31), "BITMAPS with non-ROM-font handle", handle);
            (void)ii;
            break;
        case kPoints:
            back_.fillCircle(x, y, float(pointS_) / 16.0f);
            break;
        case kLines:
            if (nvert_ & 1) back_.fillCapsule(vx_ + 0.5f, vy_ + 0.5f, x + 0.5f, y + 0.5f, lr > 0.5f ? lr - 0.5f : 0.5f);
            break;
        case kLineStrip:
            if (nvert_) back_.fillCapsule(vx_ + 0.5f, vy_ + 0.5f, x + 0.5f, y + 0.5f, lr > 0.5f ? lr - 0.5f : 0.5f);
            break;
        case kEdgeStripR:
        case kEdgeStripL:
        case kEdgeStripA:
        case kEdgeStripB:
            if (nvert_) edgeSegment(vx_, vy_, x, y);
            break;
        case kRects:
            if (nvert_ & 1) {
                // Corners are rounded by LINE_WIDTH; at the default width (16)
                // the rectangle covers x0..x1 / y0..y1 inclusive.
                float x0 = vx_, y0 = vy_, x1 = x, y1 = y;
                if (x1 < x0) std::swap(x0, x1);
                if (y1 < y0) std::swap(y0, y1);
                if (lineW_ <= 16) back_.fillRect(int(x0 + 0.5f), int(y0 + 0.5f), int(x1 + 0.5f) + 1, int(y1 + 0.5f) + 1);
                else back_.fillRoundRect(x0 + 0.5f, y0 + 0.5f, x1 + 0.5f, y1 + 0.5f, lr - 0.5f);
            }
            break;
        default:
            warnOnce(2, "VERTEX outside BEGIN/END", prim_);
            break;
    }
    vx_ = x;
    vy_ = y;
    ++nvert_;
}

void SwDisplay::text(int16_t x, int16_t y, uint8_t font, uint16_t options, const char* s) {
    if (font < 16 && handles_[font].valid && handles_[font].metric) {
        // CMD_TEXT on a custom font: widths from the RAM_G metric block.
        if (!s) return;
        const Handle& h = handles_[font];
        int w = 0;
        for (const char* p = s; *p; ++p) w += ramg_[h.metric + (uint8_t(*p) & 0x7F)];
        int px = x + translateX_ / 16, py = y;
        if (options & kOptRightX) px -= w;
        else if (options & kOptCenterX) px -= w / 2;
        if (options & kOptCenterY) py -= h.layoutH / 2;
        applyClip();
        for (const char* p = s; *p; ++p) {
            const uint8_t ch = uint8_t(*p) & 0x7F;
            const uint8_t adv = ramg_[h.metric + ch];
            if (adv && ch != ' ') customGlyph(px, py, font, ch);   // no cell below `first`: width 0
            px += adv;
        }
        return;
    }
    const RomFont* f = romFont(font);
    if (!f) {
        warnOnce(3, "CMD_TEXT with non-ROM font", font);
        return;
    }
    if (!s) return;
    int px = x + translateX_ / 16, py = y;   // the coprocessor's vertices are translated too
    const int w = textWidth(font, s);
    if (options & kOptRightX) px -= w;
    else if (options & kOptCenterX) px -= w / 2;
    if (options & kOptCenterY) py -= f->height / 2;
    applyClip();
    back_.text(px, py, font, s);
}

void SwDisplay::number(int16_t x, int16_t y, uint8_t font, uint16_t options, int32_t n) {
    char buf[24];
    const int digits = options & 0xFF;
    bool neg = false;
    uint32_t v;
    if ((options & kOptSigned) && n < 0) {
        neg = true;
        v = uint32_t(-(int64_t)n);
    } else {
        v = uint32_t(n);
    }
    char tmp[16];
    int len = std::snprintf(tmp, sizeof(tmp), "%u", v);
    int p = 0;
    if (neg) buf[p++] = '-';
    for (int i = len; i < digits && p < 20; ++i) buf[p++] = '0';
    std::memcpy(buf + p, tmp, size_t(len) + 1);
    text(x, y, font, uint16_t(options & ~0xFF), buf);
}

void SwDisplay::swap() {
    std::lock_guard<std::mutex> lk(frontMu_);
    std::memcpy(front_.data(), back_.pixels(), front_.size() * 4);
    ++frames_;
}

bool SwDisplay::readPixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t* rgb) {
    if (uint32_t(x) + w > uint32_t(kW) || uint32_t(y) + h > uint32_t(kH)) return false;
    std::lock_guard<std::mutex> lk(frontMu_);
    for (uint16_t r = 0; r < h; ++r)
        for (uint16_t c = 0; c < w; ++c) rgb[size_t(r) * w + c] = front_[size_t(y + r) * kW + x + c] & 0xFFFFFFu;
    return true;
}

bool SwDisplay::loadJpeg(uint32_t dest, ot::IStorage& fs, const char* path, uint16_t maxW, uint16_t maxH,
                         uint16_t& w, uint16_t& h) {
    w = h = 0;
    uint16_t pw = 0, ph = 0;
    uint32_t size = 0;
    if (!probeJpeg(fs, path, dest, maxW, maxH, pw, ph, size)) return false;
    std::vector<uint8_t> file(size);
    const ot::IStorage::Handle f = fs.open(path, ot::IStorage::kRead);
    if (f < 0) return false;
    uint32_t got = 0;
    while (got < size) {
        const int32_t r = fs.read(f, file.data() + got, size - got);
        if (r <= 0) break;
        got += uint32_t(r);
    }
    fs.close(f);
    std::vector<uint8_t> rgb;
    int dw = 0, dh = 0;
    if (got != size || !decodeJpegRgb(file.data(), size, rgb, dw, dh) || dw != pw || dh != ph) return false;
    uint8_t* out = ramg_.data() + dest;     // probeJpeg checked dest + 2*w*h <= RAM_G size
    for (size_t i = 0; i < size_t(dw) * size_t(dh); ++i) {
        const uint16_t c = uint16_t(((rgb[i * 3] >> 3) << 11) | ((rgb[i * 3 + 1] >> 2) << 5) | (rgb[i * 3 + 2] >> 3));
        out[i * 2] = uint8_t(c);
        out[i * 2 + 1] = uint8_t(c >> 8);
    }
    w = pw;
    h = ph;
    return true;
}

bool SwDisplay::writeRamG(uint32_t addr, const void* src, uint32_t len) {
    if (!src || addr >= kRamGSize || len > kRamGSize - addr) return false;
    std::memcpy(ramg_.data() + addr, src, len);
    return true;
}

void SwDisplay::drawRgb332(int16_t x, int16_t y, uint32_t addr, uint16_t w, uint16_t h, uint8_t scale) {
    if (!w || !h || !scale || addr + uint32_t(w) * h > kRamGSize) {
        warnOnce(5, "drawRgb332 outside RAM_G", int(addr));
        return;
    }
    // Clip: scissor intersected with the screen.
    const int cx0 = std::max<int>(scX_, 0), cy0 = std::max<int>(scY_, 0);
    const int cx1 = std::min<int>(scX_ + scW_, kW), cy1 = std::min<int>(scY_ + scH_, kH);
    const int x0 = std::max(int(x), cx0), y0 = std::max(int(y), cy0);
    const int x1 = std::min(int(x) + int(w) * scale, cx1), y1 = std::min(int(y) + int(h) * scale, cy1);
    const uint32_t a = a_;
    if (!a) return;
    uint32_t* px = back_.pixels();
    const uint8_t* src = ramg_.data() + addr;
    for (int py = y0; py < y1; ++py) {
        const uint8_t* row = src + size_t((py - y) / scale) * w;
        for (int qx = x0; qx < x1; ++qx) {
            const uint8_t c = row[(qx - x) / scale];
            // RGB332 expanded to 8 bits per channel, then modulated.
            uint32_t r = uint32_t(c >> 5) * 255 / 7, g = uint32_t((c >> 2) & 7) * 255 / 7, b = uint32_t(c & 3) * 255 / 3;
            r = r * ((rgb_ >> 16) & 255) / 255;
            g = g * ((rgb_ >> 8) & 255) / 255;
            b = b * (rgb_ & 255) / 255;
            uint32_t& d = px[size_t(py) * kW + qx];
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

void SwDisplay::drawImage(int16_t x, int16_t y, uint32_t addr, uint16_t w, uint16_t h) {
    if (!w || !h || (addr & 1u) || addr + uint32_t(w) * h * 2u > kRamGSize) {
        warnOnce(4, "drawImage outside RAM_G", int(addr));
        return;
    }
    applyClip();
    back_.blitRgb565(x, y, ramg_.data() + addr, w, h);
}

void SwDisplay::dimFrame(uint32_t* px, size_t n, uint8_t duty) {
    if (duty >= 128) return;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t p = px[i];
        const uint32_t r = ((p >> 16) & 0xFF) * duty / 128, g = ((p >> 8) & 0xFF) * duty / 128,
                       b = (p & 0xFF) * duty / 128;
        px[i] = (p & 0xFF000000u) | (r << 16) | (g << 8) | b;
    }
}

uint32_t SwDisplay::frontCopy(uint32_t* dst) const {
    std::lock_guard<std::mutex> lk(frontMu_);
    std::memcpy(dst, front_.data(), front_.size() * 4);
    return frames_;
}

}  // namespace sim
