// OpenTracker simulator -- IDisplay implemented by a software FT81x renderer.
//
// Interprets the display-list / coprocessor subset that IDisplay exposes
// (public FT81x programmer guide semantics): CLEAR_COLOR_RGB, CLEAR (colour,
// scissored), COLOR_RGB/COLOR_A (alpha blending SRC_ALPHA/ONE_MINUS_SRC_ALPHA),
// LINE_WIDTH/POINT_SIZE (radii, 1/16 px), SCISSOR_XY/SIZE, BEGIN/END with
// POINTS, LINES, LINE_STRIP, EDGE_STRIP_R/L/A/B, RECTS and BITMAPS (ROM font
// handles 16..31 only), VERTEX2F/VERTEX2II, CMD_TEXT and CMD_NUMBER, plus
// loadJpeg/drawImage (CMD_LOADIMAGE into a RAM_G model, RGB565 bitmaps).
// Commands rasterise immediately into a back buffer; swap() publishes it.
// The panel's 180-degree REG_ROTATE is not applied: the image is upright.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "canvas.h"
#include "hal_api.h"

namespace sim {

class SwDisplay final : public ot::IDisplay {
public:
    static constexpr int kW = 800, kH = 480;
    SwDisplay();

    uint16_t width() const override { return kW; }
    uint16_t height() const override { return kH; }
    void dlStart() override;
    void clearColorRGB(uint8_t r, uint8_t g, uint8_t b) override;
    void clear(bool color, bool stencil, bool tag) override;
    void colorRGB(uint8_t r, uint8_t g, uint8_t b) override;
    void colorA(uint8_t a) override;
    void lineWidth(uint16_t width16) override;
    void pointSize(uint16_t size16) override;
    void scissorXY(uint16_t x, uint16_t y) override;
    void scissorSize(uint16_t w, uint16_t h) override;
    void begin(Prim prim) override;
    void end() override;
    void vertex2f(int32_t x16, int32_t y16) override;
    void vertex2ii(uint16_t x, uint16_t y, uint8_t handle, uint8_t cell) override;
    void text(int16_t x, int16_t y, uint8_t font, uint16_t options, const char* s) override;
    void number(int16_t x, int16_t y, uint8_t font, uint16_t options, int32_t n) override;
    void swap() override;
    // CMD_GRADIENT over the scissor area (opaque, colour/alpha unchanged),
    // VERTEX_TRANSLATE_X (applied to VERTEX2F and VERTEX2II) and a 4-deep
    // SAVE_CONTEXT / RESTORE_CONTEXT stack (colour, alpha, line width, point
    // size, scissor, clear colour, translate, bitmap transform).
    // Custom fonts like the FT81x driver: bindFontSet uploads the generated
    // image (ot::fontdata::kBlob) into the RAM_G model once and returns true;
    // every dlStart sets up handles kFontHandleBase.. from the metric blocks
    // in RAM_G (format, stride, cell size, char-0 address) and the icon
    // sheet on kIconHandle. BITMAPS / CMD_TEXT on those handles draw the L4
    // cells from RAM_G (alpha = nibble * 17), honouring BITMAP_TRANSFORM and
    // bitmapSize.
    bool bindFontSet(int set) override;
    void bitmapTransform(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f) override;
    void bitmapSize(uint8_t handle, uint16_t w, uint16_t h) override;
    void gradient(int16_t x0, int16_t y0, uint32_t rgb0, int16_t x1, int16_t y1, uint32_t rgb1) override;
    void vertexTranslateX(int32_t x16) override { translateX_ = x16; }
    void saveContext() override;
    void restoreContext() override;
    bool readPixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t* rgb) override;
    // CMD_LOADIMAGE stand-in (spec 13 §G.6): same validation as the device
    // (IDisplay::probeJpeg), decoded on the host and stored as RGB565 in a
    // 1 MiB RAM_G model; drawImage() reads it back from there.
    bool loadJpeg(uint32_t ramgAddr, ot::IStorage& fs, const char* path, uint16_t maxW, uint16_t maxH,
                  uint16_t& w, uint16_t& h) override;
    void drawImage(int16_t x, int16_t y, uint32_t ramgAddr, uint16_t w, uint16_t h) override;
    // Games (spec 14 §3): raw RAM_G writes and the magnified RGB332 frame
    // (nearest filtering, scissored, modulated by COLOR_RGB/COLOR_A).
    bool writeRamG(uint32_t ramgAddr, const void* src, uint32_t len) override;
    void drawRgb332(int16_t x, int16_t y, uint32_t ramgAddr, uint16_t w, uint16_t h, uint8_t scale) override;
    const uint8_t* ramG() const { return ramg_.data(); }
    // REG_PWM_DUTY stand-in (0..128). Only the interactive window is dimmed
    // (dimFrame); screenshots/readPixels show the frame itself, like the
    // FT81x snapshot, which is taken before the backlight.
    void setBacklight(uint8_t level) override { backlight_ = level > 128 ? 128 : level; }
    uint8_t backlight() const { return backlight_; }
    // Scales an ARGB8888 frame by the backlight duty (128 = unchanged).
    static void dimFrame(uint32_t* px, size_t n, uint8_t duty);

    // Copy of the last swapped frame (ARGB8888, kW*kH). Returns the frame count.
    uint32_t frontCopy(uint32_t* dst) const;
    uint32_t frames() const { return frames_; }

private:
    void vertex(float x, float y, uint8_t handle, uint8_t cell, bool ii);
    void applyClip();
    void applyColor();
    void edgeSegment(float x0, float y0, float x1, float y1);
    void warnOnce(int key, const char* msg, int v);
    void setupFontHandles();
    void customGlyph(int x, int y, uint8_t handle, uint8_t cell);

    // FT81x bitmap handle state (custom handles only) and font table.
    struct Handle {
        bool valid = false;
        uint32_t source = 0;
        uint16_t stride = 0, layoutH = 0, sizeW = 0, sizeH = 0;
        uint32_t metric = 0;   // RAM_G address of the legacy metric block, 0 = not a font
    };
    Handle handles_[16];
    bool fontsUploaded_ = false, fontsBound_ = false;
    int fontSet_ = 0;
    int32_t xform_[6] = {256, 0, 0, 0, 256, 0};
    Canvas back_;
    std::vector<uint8_t> ramg_;
    std::vector<uint32_t> front_;
    mutable std::mutex frontMu_;
    uint32_t frames_ = 0;
    uint8_t backlight_ = 128;
    // graphics context
    uint32_t clearRgb_ = 0, rgb_ = 0xFFFFFF;
    uint8_t a_ = 255;
    uint16_t lineW_ = 16, pointS_ = 16;
    uint16_t scX_ = 0, scY_ = 0, scW_ = 2048, scH_ = 2048;
    int32_t translateX_ = 0;   // 1/16 px
    struct Context {
        uint32_t clearRgb, rgb;
        uint8_t a;
        uint16_t lineW, pointS, scX, scY, scW, scH;
        int32_t translateX;
        int32_t xform[6];
    };
    Context ctxStack_[4] = {};
    int ctxDepth_ = 0;
    int prim_ = 0;
    int nvert_ = 0;
    float vx_ = 0, vy_ = 0;
    bool warned_[64] = {};
};

}  // namespace sim
