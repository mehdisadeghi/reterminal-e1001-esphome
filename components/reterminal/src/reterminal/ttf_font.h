#pragma once

// On-device TrueType rasterizer behind ESPHome's BaseFont interface.
//
// ESPHome bakes every font as bitmaps at build time, so each size costs its
// own copy in flash and only the compiled sizes can ever be drawn. The
// status bar's size is a user setting, which made that a ladder of fixed
// steps. Carrying the outlines instead (ttf_data.h, ~32 KB for both faces)
// and rasterizing on demand makes any size free — cheaper than the four
// ladder rungs it replaces.
//
// Rasterizing is not cached: a status bar is ~40 glyphs at a few
// milliseconds each, against an ePaper refresh measured in seconds. measure()
// never rasterizes at all — it reads advances straight from hmtx, which is
// what makes the bar's fit-and-give-way pass cheap enough to run repeatedly.
//
// Hinting is absent (stb_truetype ignores it), so these faces are meant for
// display sizes. The small compiled faces still render body text, where
// hinting is what keeps 1-2 px stems crisp.

#include "esphome/components/display/display.h"

#include "arabic_shape.h"
#include "stb_truetype.h"
#include "ttf_data.h"

namespace reterminal {

class TtfFont : public esphome::display::BaseFont {
 public:
  explicit TtfFont(const uint8_t *ttf) : ttf_(ttf) {}

  void set_size(int px) { px_ = px; }

  void print(int x, int y, esphome::display::Display *display, esphome::Color color,
             const char *text, esphome::Color background) override {
    if (!ready_())
      return;
    float s = scale_();
    int pen = x;
    for (const char *p = text; *p != 0;) {
      uint32_t cp = utf8_next(p);
      int adv, lsb;
      stbtt_GetCodepointHMetrics(&info_, (int) cp, &adv, &lsb);
      int gw = 0, gh = 0, xoff = 0, yoff = 0;
      unsigned char *bmp =
          stbtt_GetCodepointBitmap(&info_, 0, s, (int) cp, &gw, &gh, &xoff, &yoff);
      if (bmp != nullptr) {
        // yoff is relative to the baseline and negative above it
        int gy0 = y + baseline_() + yoff, gx0 = pen + xoff;
        for (int row = 0; row < gh; row++)
          for (int col = 0; col < gw; col++)
            // 1-bit panel: coverage is a yes/no decision at the halfway mark
            if (bmp[row * gw + col] >= 128)
              display->draw_pixel_at(gx0 + col, gy0 + row, color);
        stbtt_FreeBitmap(bmp, nullptr);
      }
      pen += (int) (adv * s + 0.5f);
    }
  }

  void measure(const char *str, int *width, int *x_offset, int *baseline,
               int *height) override {
    *width = *x_offset = *baseline = *height = 0;
    if (!ready_())
      return;
    float s = scale_();
    *baseline = baseline_();
    *height = height_();
    int x = 0, min_x = 0;
    bool first = true;
    for (const char *p = str; *p != 0;) {
      uint32_t cp = utf8_next(p);
      int adv, lsb;
      stbtt_GetCodepointHMetrics(&info_, (int) cp, &adv, &lsb);
      int off = (int) (lsb * s);
      min_x = first ? off : (x + off < min_x ? x + off : min_x);
      first = false;
      x += (int) (adv * s + 0.5f);
    }
    *x_offset = min_x;
    *width = x - min_x;
  }

 private:
  // Parsing the face is deferred: these live as globals, and the tables are
  // only walked once something actually asks to draw.
  bool ready_() {
    if (!init_) {
      init_ = stbtt_InitFont(&info_, ttf_, stbtt_GetFontOffsetForIndex(ttf_, 0)) != 0;
      if (init_)
        stbtt_GetFontVMetrics(&info_, &ascent_, &descent_, &line_gap_);
    }
    return init_;
  }
  // Em-to-pixels, not pixel-height: ESPHome's "size: N" sets the em square
  // to N px, and matching it keeps a given number meaning the same thing as
  // it did with the compiled faces — and the same thing in both languages,
  // whose ascent/descent ratios differ widely.
  float scale_() { return stbtt_ScaleForMappingEmToPixels(&info_, (float) px_); }
  int baseline_() { return (int) (ascent_ * scale_() + 0.5f); }
  int height_() { return (int) ((ascent_ - descent_) * scale_() + 0.5f); }

  const uint8_t *ttf_;
  stbtt_fontinfo info_{};
  int px_ = 20;
  int ascent_ = 0, descent_ = 0, line_gap_ = 0;
  bool init_ = false;
};

}  // namespace reterminal
