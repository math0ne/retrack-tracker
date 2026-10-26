// OpenTracker simulator -- software raster canvas (ARGB8888) with FT81x-like
// primitives and the ROM-font look-alikes. Used both for the emulated display
// and for drawing the front panel around it.
#pragma once

#include <cstdint>
#include <vector>

namespace sim {

struct RomFont {
    uint8_t height;
    uint8_t baseline;
    const uint8_t* widths;    // 95 entries, chars 32..126
    const uint32_t* offsets;  // into px
    const uint8_t* px;        // width*height alpha bytes per glyph
};

// Font handle 16..31 (FT81x ROM font numbering); nullptr otherwise.
const RomFont* romFont(uint8_t handle);
int textWidth(uint8_t handle, const char* s);

class Canvas {
public:
    Canvas(int w, int h) : w_(w), h_(h), px_(size_t(w) * size_t(h), 0xFF000000u) {}
    int width() const { return w_; }
    int height() const { return h_; }
    uint32_t* pixels() { return px_.data(); }
    const uint32_t* pixels() const { return px_.data(); }

    // Clip rectangle (half-open), always intersected with the canvas.
    void setClip(int x0, int y0, int x1, int y1);
    void resetClip() { setClip(0, 0, w_, h_); }
    // Current colour 0xRRGGBB and alpha 0..255 for all drawing below.
    void setColor(uint32_t rgb, uint8_t a = 255) { rgb_ = rgb & 0xFFFFFF; a_ = a; }

    void clearRect(uint32_t rgb);                          // fill clip, no blending
    void blendPixel(int x, int y, uint32_t cov);           // cov 0..255
    void fillRect(int x0, int y0, int x1, int y1);         // half-open
    // Rectangle with rounded corners: area within r of [x0,x1]x[y0,y1] (float px).
    void fillRoundRect(float x0, float y0, float x1, float y1, float r);
    void fillCircle(float cx, float cy, float r);
    void fillCapsule(float x0, float y0, float x1, float y1, float r);  // thick line
    void glyph(int x, int y, uint8_t handle, uint8_t ch);
    // RGB565 bitmap (little-endian, stride 2*w) with its top-left at (x, y),
    // modulated by the current colour and alpha like an FT81x BITMAPS draw.
    void blitRgb565(int x, int y, const uint8_t* src, int w, int h);
    // FT81x L4 bitmap cell (two pixels per byte, left pixel in the high
    // nibble, alpha = nibble * 17) drawn as a drawW x drawH rectangle at
    // (x, y): BITMAP_TRANSFORM t[0..5] = A..F (8.8 / 1/256 px), NEAREST
    // sampling at pixel centres, BORDER (transparent) outside srcW x srcH.
    // Modulated by the current colour and alpha.
    void blitL4(int x, int y, const uint8_t* src, int stride, int srcW, int srcH, int drawW, int drawH,
                const int32_t t[6]);
    // Draws text with its cell top-left at (x, y) (no options).
    void text(int x, int y, uint8_t handle, const char* s);

private:
    int w_, h_;
    std::vector<uint32_t> px_;
    int cx0_ = 0, cy0_ = 0, cx1_ = 0, cy1_ = 0;
    uint32_t rgb_ = 0xFFFFFF;
    uint8_t a_ = 255;
};

}  // namespace sim
