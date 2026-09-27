#pragma once

// Page renderers for the 800x480 ePaper.

#include <algorithm>

#include "esphome/components/display/display.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "device.h"
#include "dial_image.h"
#include "khayyam_fa.h"
#include "ttf_font.h"
#include "hijri.h"
#include "jalali.h"
#include "lang.h"
#include "pure.h"

namespace reterminal {

using esphome::display::BaseFont;
using esphome::display::Display;
using esphome::display::TextAlign;

// Font roles, resolved through the active language's registered set —
// draw code says lfont(F_MEDIUM) and never branches on the language.
enum FontRole { F_TINY, F_SMALL, F_SMALLB, F_MEDIUM, F_LARGE, F_TIME, F_HUGE, F_ROLES };
static BaseFont *fonts_[lang::LANG_COUNT][F_ROLES];

// lfont, not F: Arduino's F() flash-string macro owns that name
inline BaseFont *lfont(int role) { return fonts_[lang::lang_idx][role]; }

inline void register_fonts(int li, BaseFont *tiny, BaseFont *small_f, BaseFont *smallb,
                           BaseFont *med, BaseFont *large, BaseFont *time_f, BaseFont *huge) {
  BaseFont *set[F_ROLES] = {tiny, small_f, smallb, med, large, time_f, huge};
  for (int i = 0; i < F_ROLES; i++)
    fonts_[li][i] = set[i];
}

// The status bar is the one surface whose type size is a user setting. Its
// faces are outlines rasterized on demand rather than a compiled role, so
// every size in the range exists and none of them costs flash.
static TtfFont bar_faces_[lang::LANG_COUNT] = {TtfFont(INTER_BOLD_TTF),
                                               TtfFont(VAZIRMATN_TTF)};
static_assert(lang::LANG_COUNT == 2, "one status-bar face per language");

inline BaseFont *bar_font() {
  TtfFont &f = bar_faces_[lang::lang_idx];
  f.set_size(bar_font_px);
  return &f;
}

inline void lang_apply(const char *code) { lang::lang_idx = lang::lang_index(code); }

// Runtime labels may be in another script than the UI language: an English
// UI can hold a Persian zone label. Pick a font set that can render the
// text — the Persian set covers Latin too, the Inter set is ASCII-only.
inline BaseFont *label_font(int role, const char *text) {
  for (const char *p = text; *p != 0; p++)
    if ((uint8_t) *p >= 0x80)
      return fonts_[lang::lang_index("fa")][role];
  return lfont(role);
}

// --- Pages ----------------------------------------------------------------

// Small sun (day) / crescent moon (night) glyph; bg carves the crescent so
// it works on normal and inverted rows alike.
inline void thick_line(Display &it, int x1, int y1, int x2, int y2, int w, esphome::Color color);

// RTL mirroring for the text pages: mx() flips an x anchor, ma() swaps the
// horizontal alignment. Both collapse to identity in the English build.
inline int mx(int x) { return lang::rtl() ? 800 - x : x; }

inline TextAlign ma(TextAlign a) {
  if (!lang::rtl())
    return a;
  switch (a) {
    case TextAlign::TOP_LEFT:
      return TextAlign::TOP_RIGHT;
    case TextAlign::TOP_RIGHT:
      return TextAlign::TOP_LEFT;
    case TextAlign::CENTER_LEFT:
      return TextAlign::CENTER_RIGHT;
    case TextAlign::CENTER_RIGHT:
      return TextAlign::CENTER_LEFT;
    default:
      return a;
  }
}

inline void draw_day_night(Display &it, int cx, int cy, bool night, esphome::Color fg,
                           esphome::Color bg) {
  if (night) {
    it.filled_circle(cx, cy, 10, fg);
    it.filled_circle(cx + 6, cy - 4, 8, bg);
  } else {
    it.filled_circle(cx, cy, 6, fg);
    for (int k = 0; k < 8; k++) {
      float a = k * 3.14159265f / 4.0f;
      thick_line(it, cx + (int) (cosf(a) * 9.0f), cy + (int) (sinf(a) * 9.0f),
                 cx + (int) (cosf(a) * 13.0f), cy + (int) (sinf(a) * 13.0f), 2, fg);
    }
  }
}

inline void draw_world_clock(Display &it) {
  BaseFont *time_f = lfont(F_TIME), *med = lfont(F_MEDIUM), *small_f = lfont(F_SMALL);
  time_t now = ::time(nullptr);
  it.print(400, 12, med, TextAlign::TOP_CENTER, lang::L().title_clock);
  it.line(20, 55, 780, 55);
  if (now < MIN_VALID_EPOCH) {
    it.print(400, 220, med, TextAlign::CENTER, lang::L().wait_sync);
    return;
  }
  if (zone_count == 0) {
    it.print(400, 220, med, TextAlign::CENTER, lang::L().no_zones);
    return;
  }
  bool has_home = home_zone >= 0 && home_zone < zone_count;
  // without a home zone nothing is inverted and offsets are vs. UTC
  int base_off = has_home ? zone_offset_min(zones[home_zone], now) : 0;
  // rows keep their designed height while the content row can hold five of
  // them, and give it up only when a taller status bar leaves less
  const int TOP = 58;
  int RH = std::min(76, (content_bottom(page) - TOP) / MAX_ZONES);
  int y = TOP + (MAX_ZONES - zone_count) * (RH / 2);  // vertically centered
  for (int zi = 0; zi < zone_count; zi++) {
    const Zone &z = zones[zi];
    int off = zone_offset_min(z, now);
    time_t local = now + (time_t) off * 60;
    struct tm lt;
    gmtime_r(&local, &lt);
    char buf[64], loc[24];  // buf holds shaped dates: 3-byte forms
    bool is_local = zi == home_zone;
    auto fg = is_local ? esphome::display::COLOR_OFF : esphome::display::COLOR_ON;
    int cy = y + RH / 2;
    if (is_local)
      it.filled_rectangle(0, y, 800, RH);
    const char *cname = lang::zone_name(z);
    it.print(mx(40), cy, label_font(F_MEDIUM, cname), fg, ma(TextAlign::CENTER_LEFT), cname);
    if (!is_local) {
      int rel = off - base_off;
      int a = rel < 0 ? -rel : rel;
      snprintf(buf, sizeof(buf), "%c%d:%02d", rel < 0 ? '-' : '+', a / 60, a % 60);
      lang::num(loc, sizeof(loc), buf);
      it.print(mx(430), cy + 4, small_f, fg, ma(TextAlign::CENTER_RIGHT), loc);
    }
    lang::date_short(buf, sizeof(buf), lt.tm_wday, lt.tm_mday, lt.tm_mon);
    it.print(mx(580), cy + 4, small_f, fg, ma(TextAlign::CENTER_RIGHT), buf);
    strftime(buf, sizeof(buf), "%H:%M", &lt);
    lang::num(loc, sizeof(loc), buf);
    it.print(mx(726), cy, time_f, fg, ma(TextAlign::CENTER_RIGHT), loc);
    draw_day_night(it, mx(762), cy, is_night(lt.tm_hour), fg,
                   is_local ? esphome::display::COLOR_ON : esphome::display::COLOR_OFF);
    // hairline between plain rows; the inverted band is its own separator
    if (zi < zone_count - 1 && !is_local && zi + 1 != home_zone)
      it.line(40, y + RH, 760, y + RH);
    y += RH;
  }
}

// Both status-bar icons are drawn from the same small integer grid times s.
// The extra passes offset by one pixel thicken every stroke, diagonals and
// arcs included, so a doubled bar does not leave them spindly.
inline void draw_sd_icon(Display &it, int x, int y, int s) {
  auto c = esphome::display::COLOR_OFF;
  for (int t = 0; t < s; t++) {
    int ox = x + t, oy = y + t;
    it.line(ox + 3 * s, oy, ox + 9 * s, oy, c);  // top edge, cut by the corner notch
    it.line(ox + 9 * s, oy, ox + 9 * s, oy + 13 * s, c);
    it.line(ox, oy + 13 * s, ox + 9 * s, oy + 13 * s, c);
    it.line(ox, oy + 3 * s, ox, oy + 13 * s, c);
    it.line(ox, oy + 3 * s, ox + 3 * s, oy, c);
  }
  for (int px = x + 2 * s; px <= x + 8 * s; px += 2 * s)
    it.line(px, y + 2 * s, px, y + 4 * s, c);
}

// Lit bulb = the device is held awake (Pause Deep Sleep or keep-awake helper)
inline void draw_awake_icon(Display &it, int x, int y, int s) {
  auto c = esphome::display::COLOR_OFF;
  for (int t = 0; t < s; t++) {
    int ox = x + t, oy = y + t;
    it.line(ox + 5 * s, oy, ox + 5 * s, oy + 1 * s, c);  // rays
    it.line(ox, oy + 2 * s, ox + 1 * s, oy + 3 * s, c);
    it.line(ox + 9 * s, oy + 3 * s, ox + 10 * s, oy + 2 * s, c);
    it.circle(x + 5 * s, y + 8 * s, 4 * s - t, c);  // glass
    it.line(ox + 3 * s, oy + 14 * s, ox + 7 * s, oy + 14 * s, c);  // base
    it.line(ox + 3 * s, oy + 16 * s, ox + 7 * s, oy + 16 * s, c);
  }
}

// Bottom status bar, inverted. Start side: the weekday, then the enabled
// calendars as one box (Solar Hijri, Lunar Hijri, Gregorian last, always
// on); end side, growing inward: battery, awake bulb, SD-card icon, RTC
// warning, device climate.
//
// Nothing here is sized by hand. Both groups are measured at whatever face
// the user chose, and while the row cannot hold them the lowest-value items
// give way in turn — the battery percentage, climate, the RTC warning, the
// two icons, then the optional calendars. Weekday and Gregorian date always
// survive. So a larger type size costs detail rather than producing overlap
// or text running off the panel.
//
// The percentage leads that list only when the hairline gauge is on, since
// the edge already carries the same reading; without the gauge it is the
// only battery indication there is and it stays.
inline void draw_status_bar(Display &it, float battery_pct, float dev_t, float dev_h,
                            bool show_climate, bool sd_present, bool show_jalali,
                            bool show_lunar, bool awake_hold, bool batt_gauge) {
  BaseFont *small_f = bar_font();
  int s = bar_scale(), by0 = bar_top(), base = bar_mid();
  it.filled_rectangle(0, by0, 800, bar_h());
  time_t now = ::time(nullptr);
  auto astart = ma(TextAlign::CENTER_LEFT);
  auto aend = ma(TextAlign::CENTER_RIGHT);
  int dir = lang::rtl() ? -1 : 1;
  int margin = bar_px(20), gap = bar_px(24);
  auto text_w = [&](const char *t) {
    int bx, by, bw, bh;
    it.get_text_bounds(0, base, t, small_f, astart, &bx, &by, &bw, &bh);
    return bw;
  };

  // shaped Persian dates run ~60 bytes (presentation forms are 3-byte UTF-8)
  char dates[4][96], climate[96], batt[96];
  bool on_jalali = false, on_lunar = false;
  int n_dates = 0, i_jalali = -1, i_lunar = -1;
  if (now > MIN_VALID_EPOCH) {
    struct tm lt;
    localtime_r(&now, &lt);
    snprintf(dates[n_dates++], sizeof(dates[0]), "%s", lang::L().weekdays[lt.tm_wday]);
    if (show_jalali) {
      int jy, jm, jd;
      jalali::from_gregorian(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, jy, jm, jd);
      i_jalali = n_dates;
      on_jalali = true;
      lang::d_m_y(dates[n_dates++], sizeof(dates[0]), jd, lang::jalali_month(jm - 1), jy);
    }
    if (show_lunar) {
      int hy, hm, hd;
      hijri::from_gregorian(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, hy, hm, hd);
      i_lunar = n_dates;
      on_lunar = true;
      lang::d_m_y(dates[n_dates++], sizeof(dates[0]), hd, lang::hijri_month(hm - 1), hy);
    }
    lang::d_m_y(dates[n_dates++], sizeof(dates[0]), lt.tm_mday, lang::L().months[lt.tm_mon],
                lt.tm_year + 1900);
  }

  bool on_batt = !std::isnan(battery_pct);
  bool on_bulb = awake_hold, on_sd = sd_present, on_rtc = rtc_bad;
  bool on_climate = show_climate && !std::isnan(dev_t) && !std::isnan(dev_h);
  if (on_batt) {
    char pct[8];
    snprintf(pct, sizeof(pct), "%.0f", battery_pct);
    lang::percent(batt, sizeof(batt), pct);
  }
  if (on_climate)
    lang::climate(climate, sizeof(climate), dev_t, dev_h);

  // Advances, not glyph widths: each carries the spacing that follows it.
  auto row_w = [&]() {
    int w = 0, shown = 0;
    for (int i = 0; i < n_dates; i++) {
      if ((i == i_jalali && !on_jalali) || (i == i_lunar && !on_lunar))
        continue;
      w += text_w(dates[i]);
      shown++;
    }
    if (shown > 1)
      w += gap * (shown - 1);
    if (on_batt)
      w += bar_px(58);
    if (on_bulb)
      w += bar_px(24);
    if (on_sd)
      w += bar_px(22);
    if (on_rtc)
      w += bar_px(54);
    if (on_climate)
      w += text_w(climate) + gap;
    return w;
  };
  bool *give_way[] = {&on_batt,   &on_climate, &on_rtc,   &on_sd,
                      &on_bulb,   &on_lunar,   &on_jalali};
  int room = 800 - 2 * margin;
  for (int i = batt_gauge ? 0 : 1; i < (int) (sizeof(give_way) / sizeof(*give_way)); i++) {
    if (row_w() <= room)
      break;
    *give_way[i] = false;
  }

  int lx = mx(margin);
  for (int i = 0; i < n_dates; i++) {
    if ((i == i_jalali && !on_jalali) || (i == i_lunar && !on_lunar))
      continue;
    it.print(lx, base, small_f, esphome::display::COLOR_OFF, astart, dates[i]);
    lx += dir * (text_w(dates[i]) + gap);
  }
  int x = mx(800 - margin);
  if (on_batt) {
    it.print(x, base, small_f, esphome::display::COLOR_OFF, aend, batt);
    x -= dir * bar_px(58);
  }
  if (on_bulb) {
    draw_awake_icon(it, lang::rtl() ? x : x - 11 * s, by0 + 5 * s, s);
    x -= dir * bar_px(24);
  }
  if (on_sd) {
    draw_sd_icon(it, lang::rtl() ? x : x - 10 * s, by0 + 10 * s, s);
    x -= dir * bar_px(22);
  }
  if (on_rtc) {
    it.print(x, base, small_f, esphome::display::COLOR_OFF, aend, "RTC!");
    x -= dir * bar_px(54);
  }
  if (on_climate)
    it.print(x, base, small_f, esphome::display::COLOR_OFF, aend, climate);
}

inline void draw_numbers(Display &it) {
  BaseFont *huge = lfont(F_HUGE), *large = lfont(F_LARGE), *med = lfont(F_MEDIUM), *small_f = lfont(F_SMALL);
  int en[MAX_COLS], n = 0;
  for (int i = 0; i < MAX_COLS; i++)
    if (cols[i].enabled)
      en[n++] = i;
  if (n == 0) {
    it.print(400, 220, med, TextAlign::CENTER, lang::L().no_cols);
    return;
  }
  static const int CX[3][3] = {{400, 0, 0}, {200, 600, 0}, {133, 400, 667}};
  if (n >= 2)
    it.line(n == 2 ? 400 : 267, 50, n == 2 ? 400 : 267, 400);
  if (n == 3)
    it.line(533, 50, 533, 400);
  for (int k = 0; k < n; k++) {
    const Column &c = cols[en[k]];
    int x = mx(CX[n - 1][k]);
    float t = last_val[en[k]][M_TEMP], h = last_val[en[k]][M_HUM];
    it.print(x, 40, med, TextAlign::TOP_CENTER, c.label);
    // the value is the information, the unit a caption on its shoulder
    // (on the reading side: right of the numeral in LTR, left in RTL)
    char buf[16], loc[24];
    int bx, by, bw, bh;
    if (std::isnan(t)) {
      it.print(x, 140, large, TextAlign::TOP_CENTER, "--");
    } else {
      snprintf(buf, sizeof(buf), "%.1f", t);
      lang::num(loc, sizeof(loc), buf);
      it.print(x, 110, huge, TextAlign::TOP_CENTER, loc);
      it.get_text_bounds(x, 110, loc, huge, TextAlign::TOP_CENTER, &bx, &by, &bw, &bh);
      if (lang::rtl())
        it.print(bx - 6, 122, small_f, TextAlign::TOP_RIGHT, "°C");
      else
        it.print(bx + bw + 6, 122, small_f, TextAlign::TOP_LEFT, "°C");
    }
    if (std::isnan(h)) {
      it.print(x, 290, large, TextAlign::TOP_CENTER, "--");
    } else {
      snprintf(buf, sizeof(buf), "%.0f", h);
      lang::num(loc, sizeof(loc), buf);
      it.print(x, 270, huge, TextAlign::TOP_CENTER, loc);
      it.get_text_bounds(x, 270, loc, huge, TextAlign::TOP_CENTER, &bx, &by, &bw, &bh);
      if (lang::rtl())
        it.print(bx - 6, 282, small_f, TextAlign::TOP_RIGHT, lang::L().unit_pct);
      else
        it.print(bx + bw + 6, 282, small_f, TextAlign::TOP_LEFT, lang::L().unit_pct);
    }
  }
}

// Series distinction on a 1-bit panel, papers-style: line weight + dash
// pattern + a distinct point marker per series (circle / square / triangle).
// Tight gaps (one or two segments ≈ 4-7 px): the patterns stay tellable
// apart while the curves read as continuous lines.
inline bool style_skip(int style, int i) {
  if (style == 1)
    return (i % 4) == 3;  // long dash: 3 on, 1 off
  if (style == 2)
    return (i % 6) >= 4;  // short dash: 4 on, 2 off
  return false;
}

inline void draw_marker(Display &it, int x, int y, int shape) {
  switch (shape) {
    case 0:  // filled circle
      it.filled_circle(x, y, 3);
      break;
    case 1:  // open square, 2 px border
      it.rectangle(x - 3, y - 3, 7, 7);
      it.rectangle(x - 2, y - 2, 5, 5);
      break;
    default:  // open triangle
      it.line(x - 4, y + 3, x + 4, y + 3);
      it.line(x - 4, y + 4, x + 4, y + 4);
      it.line(x - 4, y + 3, x, y - 4);
      it.line(x + 4, y + 3, x, y - 4);
      break;
  }
}

// One legend item per enabled column: swatch (line style + marker) + label,
// no values — those live on the numbers page. Items flow with a fixed gap
// (labels measured so spacing stays even), right-aligned to `right`.
inline void draw_legend(Display &it, BaseFont *f, int right, int y) {
  int total = 0;
  for (int i = 0; i < MAX_COLS; i++) {
    if (!cols[i].enabled)
      continue;
    int bx, by, bw, bh;
    it.get_text_bounds(0, 0, cols[i].label, f, TextAlign::TOP_LEFT, &bx, &by, &bw, &bh);
    total += 30 + bw + 22;
  }
  if (total == 0)
    return;
  int x = right - (total - 22);  // no trailing gap after the last item
  for (int i = 0; i < MAX_COLS; i++) {
    if (!cols[i].enabled)
      continue;
    int cy = y + 8;
    for (int seg = 0; seg < 24; seg += 2) {
      if (style_skip(i, seg / 2))
        continue;
      it.line(x + seg, cy, x + seg + 2, cy);
      if (i == 0)
        it.line(x + seg, cy + 1, x + seg + 2, cy + 1);
    }
    draw_marker(it, x + 12, cy, i);
    x += 30;
    int bx, by, bw, bh;
    it.get_text_bounds(x, y, cols[i].label, f, TextAlign::TOP_LEFT, &bx, &by, &bw, &bh);
    it.print(x, y, f, TextAlign::TOP_LEFT, cols[i].label);
    x += bw + 22;
  }
}

// Draws into the [top, bottom] band so it serves full and half pages alike.
// axis_f is the small tick-label font.
inline void draw_graph_band(Display &it, const int16_t rows[MAX_COLS][2][POINTS], int metric,
                            time_t end_t, time_t span, BaseFont *med, BaseFont *axis_f, int top,
                            int bottom, bool day_axis) {
  int mid = (top + bottom) / 2;
  float lo = NAN, hi = NAN;
  for (int c = 0; c < MAX_COLS; c++) {
    if (!cols[c].enabled)
      continue;
    for (int i = 0; i < POINTS; i++) {
      float v = pt_get(rows[c][metric][i]);
      if (std::isnan(v))
        continue;
      if (std::isnan(lo) || v < lo)
        lo = v;
      if (std::isnan(hi) || v > hi)
        hi = v;
    }
  }
  if (std::isnan(lo)) {
    it.print(400, mid, med, TextAlign::CENTER, "No data yet");
    return;
  }
  if (hi - lo < 1.0f) {
    lo -= 0.5f;
    hi += 0.5f;
  }
  const int X0 = 80, X1 = 784, Y0 = top, Y1 = bottom;
  static const float STEPS[] = {0.5f, 1, 2, 5, 10, 20, 25, 50};
  float max_lines = (Y1 - Y0) > 250 ? 6.0f : 4.0f;
  float step = STEPS[0];
  for (float s : STEPS) {
    step = s;
    if ((hi - lo) / s <= max_lines)
      break;
  }
  float glo = floorf(lo / step) * step;
  float ghi = ceilf(hi / step) * step;
  auto sx = [&](int i) { return X0 + i * (X1 - X0) / (POINTS - 1); };
  auto sy = [&](float v) { return Y1 - (int) ((v - glo) * (float) (Y1 - Y0) / (ghi - glo)); };

  for (float v = glo; v <= ghi + step / 2; v += step) {
    int y = sy(v);
    for (int x = X0; x <= X1; x += 4)
      it.draw_pixel_at(x, y);
    it.printf(X0 - 8, y, axis_f, TextAlign::CENTER_RIGHT, "%g", (double) v);
  }

  time_t start = end_t - span;
  struct tm lt;
  if (day_axis) {
    localtime_r(&start, &lt);
    time_t first = start + (4 * 3600 - ((lt.tm_hour % 4) * 3600 + lt.tm_min * 60 + lt.tm_sec));
    for (time_t t = first; t <= end_t; t += 4 * 3600) {
      int x = X0 + (int) ((t - start) * (long) (X1 - X0) / span);
      for (int y = Y0; y <= Y1; y += 4)
        it.draw_pixel_at(x, y);
      localtime_r(&t, &lt);
      it.printf(x, Y1 + 5, axis_f, TextAlign::TOP_CENTER, "%02d:00", lt.tm_hour);
    }
  } else {
    int loff = local_offset_min(end_t);
    time_t lstart = start + (time_t) loff * 60;
    time_t midnight0 = lstart - (lstart % 86400) + 86400;
    for (time_t lm = midnight0; lm < lstart + span; lm += 86400) {
      int x = X0 + (int) ((lm - lstart) * (long) (X1 - X0) / span);
      for (int y = Y0; y <= Y1; y += 4)
        it.draw_pixel_at(x, y);
    }
  }
  it.rectangle(X0, Y0, X1 - X0 + 1, Y1 - Y0 + 1);

  for (int c = 0; c < MAX_COLS; c++) {
    if (!cols[c].enabled)
      continue;
    const int16_t *row = rows[c][metric];
    for (int i = 0; i + 1 < POINTS; i++) {
      if (row[i] == PT_NAN || row[i + 1] == PT_NAN)
        continue;
      if (style_skip(c, i))
        continue;
      it.line(sx(i), sy(pt_get(row[i])), sx(i + 1), sy(pt_get(row[i + 1])));
      if (c == 0)
        it.line(sx(i), sy(pt_get(row[i])) + 1, sx(i + 1), sy(pt_get(row[i + 1])) + 1);
    }
    // point markers, phase-shifted per series so they don't stack
    for (int i = 8 + c * 8; i < POINTS; i += 24) {
      if (row[i] == PT_NAN)
        continue;
      draw_marker(it, sx(i), sy(pt_get(row[i])), c);
    }
  }
}

inline bool no_cols(Display &it) {
  BaseFont *med = lfont(F_MEDIUM);
  for (int i = 0; i < MAX_COLS; i++)
    if (cols[i].enabled)
      return false;
  it.print(400, 220, med, TextAlign::CENTER, lang::L().no_cols);
  return true;
}

inline void draw_graph_page(Display &it, int metric) {
  BaseFont *med = lfont(F_MEDIUM), *tiny_f = lfont(F_TINY);
  const char *title = metric == M_TEMP ? lang::L().title_temp : lang::L().title_hum;
  it.print(mx(20), 4, med, ma(TextAlign::TOP_LEFT), title);
  if (no_cols(it))
    return;
  draw_legend(it, tiny_f, 784, 10);
  // the band stops short of the content row's floor by the day axis it
  // prints underneath itself, and never grows past its tuned depth
  draw_graph_band(it, series_day, metric, fetch_time, SPAN, med, tiny_f, 48,
                  std::min(420, content_bottom(page) - 26), true);
}

// Combined page over week_days: temperature and humidity stacked, one
// legend, shared day axis at the bottom, vertical inverted section titles.
inline void draw_week_page(Display &it) {
  BaseFont *med = lfont(F_MEDIUM), *small_f = lfont(F_SMALLB), *tiny_f = lfont(F_TINY);
  if (no_cols(it))
    return;
  if (any_ha_col() && ha_fetch_at > MIN_VALID_EPOCH) {
    struct tm lt;
    localtime_r(&ha_fetch_at, &lt);
    it.printf(6, 6, tiny_f, TextAlign::TOP_LEFT, "data %02d:%02d", lt.tm_hour, lt.tm_min);
  }
  draw_legend(it, tiny_f, 784, 6);

  const struct {
    int metric;
    const char *tag;
    int y0, y1;
  } BANDS[2] = {{M_TEMP, "TEMP", 30, 202}, {M_HUM, "HUM", 240, 414}};

  // The stack below is authored against the normal content row; gy maps it
  // into whatever the row actually offers, and is the identity there.
  const int Y0 = 30, AUTHORED = 384;  // 30..414, with the day axis just below
  int have = std::min(AUTHORED, content_bottom(page) - 32 - Y0);
  auto gy = [&](int y) { return Y0 + (y - Y0) * have / AUTHORED; };

  for (const auto &band : BANDS) {
    int y0 = gy(band.y0), y1 = gy(band.y1);
    it.filled_rectangle(0, y0, 32, y1 - y0);
    int nt = (int) strlen(band.tag);
    int ty = (y0 + y1) / 2 - nt * 15;
    for (int i = 0; i < nt; i++) {
      char ch[2] = {band.tag[i], 0};
      it.print(16, ty + i * 30, small_f, esphome::display::COLOR_OFF, TextAlign::TOP_CENTER, ch);
    }
    draw_graph_band(it, series_week, band.metric, week_fetch_time, week_span(), med, tiny_f,
                    y0, y1, false);
  }

  // day labels centered per day, once, under the bottom band; weekday names
  // repeat past a week, so longer spans label with the day of month
  if (week_fetch_time > MIN_VALID_EPOCH) {
    const int X0 = 80, X1 = 784;
    time_t span = week_span();
    int loff = local_offset_min(week_fetch_time);
    time_t lstart = week_fetch_time - span + (time_t) loff * 60;
    time_t midnight0 = lstart - (lstart % 86400) + 86400;
    for (time_t lm = midnight0 - 86400; lm < lstart + span; lm += 86400) {
      time_t center = lm + 43200;
      if (center < lstart || center > lstart + span)
        continue;
      int x = X0 + (int) ((center - lstart) * (long) (X1 - X0) / span);
      struct tm dt;
      gmtime_r(&center, &dt);
      char day[8];
      strftime(day, sizeof(day), week_days > 7 ? "%d" : "%a", &dt);
      it.print(x, gy(424), tiny_f, TextAlign::TOP_CENTER, day);
    }
  }
}

inline void thick_line(Display &it, int x1, int y1, int x2, int y2, int w,
                       esphome::Color color) {
  float dx = (float) (x2 - x1), dy = (float) (y2 - y1);
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 1.0f)
    return;
  float px = -dy / len, py = dx / len;
  for (int k = -(w / 2); k <= w / 2; k++) {
    int ox = (int) lroundf(px * (float) k), oy = (int) lroundf(py * (float) k);
    it.line(x1 + ox, y1 + oy, x2 + ox, y2 + oy, color);
  }
}

// Five analog world clocks, layout adapts to the zone count.
inline void draw_analog_clocks(Display &it, bool show_offsets, bool labels_inside, bool photos,
                               bool night_mode) {
  BaseFont *med = lfont(F_MEDIUM);
  time_t now = ::time(nullptr);
  if (now < MIN_VALID_EPOCH) {
    it.print(400, 220, med, TextAlign::CENTER, lang::L().wait_sync);
    return;
  }
  constexpr float PI_F = 3.14159265f;
  if (zone_count == 0) {
    it.print(400, 220, med, TextAlign::CENTER, lang::L().no_zones);
    return;
  }
  // Layout engine, dial-first. Rows per count (1-3 in one row, 4 as a
  // 2+2 staircase, 5 as 2 over 3). The radius is solved from the space
  // the page offers (status bar or not, margins, battery hairline) and
  // the caption face then FOLLOWS the dial: the largest tier whose
  // measured height still leaves a dial that justifies it. Centers are
  // computed from the final radius with equal gaps, spanning the full
  // width; two-row counts sit on N evenly spaced columns, rows
  // interleaved, so no side collects leftover whitespace.
  static const struct {
    int rows, r1;  // row count and clocks in the first row
  } SHAPE[MAX_ZONES] = {{1, 1}, {1, 2}, {1, 3}, {2, 2}, {2, 2}};
  const int TOP = 6, GAP = 6, EDGE = 8, EDGE_R = 12, MINSP = 12;
  int rows = SHAPE[zone_count - 1].rows;
  int r1 = SHAPE[zone_count - 1].r1;
  int avail = content_bottom(page) - TOP;
  const int W = 800 - EDGE - EDGE_R;

  auto measure_cap = [&](int role) {
    int cap = 0;
    for (int i = 0; i < zone_count; i++) {
      const char *nm = lang::zone_name(zones[i]);
      int bx, by, bw, bh;
      it.get_text_bounds(0, 0, nm, label_font(role, nm), TextAlign::TOP_LEFT, &bx, &by, &bw,
                         &bh);
      cap = std::max(cap, bh + 4);
    }
    return cap;
  };
  // Two-row layouts let the rows overlap vertically: a bottom dial rises
  // into the top row's horizontal gap, so the binding constraint is the
  // circle distance sqrt(d^2 + dy^2) >= 2R + MINSP between neighbouring
  // columns, not stacked row blocks. Top-row captions stay UNDER their
  // dials when they fit the corridor between the rising bottom dials
  // (measured widths vs d - R); only when too wide do they move above —
  // and cost a second caption band in the vertical span. dy_out is the
  // solved distance between the two rows' centers.
  int dy = 0;
  bool caps_above = false;
  auto solve_single = [&](int cap) {
    int r = (avail - GAP - cap) / 2;
    return std::min(r, (W - (zone_count + 1) * MINSP) / (2 * zone_count));
  };
  auto solve_rows = [&](int cap, int bands, int &dy_out) {
    dy_out = 0;
    for (int r = avail / 2; r >= 20; r--) {
      int d = (W - 2 * r) / (zone_count - 1);
      if (d <= 0)
        continue;
      long need = 2L * r + MINSP;
      long dy2 = need * need - (long) d * d;
      int v = dy2 > 0 ? (int) ceilf(sqrtf((float) dy2)) : 0;
      if (2 * r + v + bands * (GAP + cap) <= avail) {
        dy_out = v;
        return r;
      }
    }
    return 20;
  };
  auto top_caption_w = [&](int role) {
    int w = 0;
    for (int i = 0; i < r1; i++) {
      const char *nm = lang::zone_name(zones[i]);
      int bx, by, bw, bh;
      it.get_text_bounds(0, 0, nm, label_font(role, nm), TextAlign::TOP_LEFT, &bx, &by, &bw,
                         &bh);
      w = std::max(w, bw);
    }
    return w;
  };

  int cap_role, CAP, R;
  if (labels_inside) {
    CAP = 0;
    R = rows == 1 ? solve_single(0) : solve_rows(0, 0, dy);
    cap_role = R >= 150 ? F_LARGE : (R >= 110 ? F_MEDIUM : F_SMALL);
  } else {
    static const int ROLES[3] = {F_LARGE, F_MEDIUM, F_SMALL};
    static const int RMIN[3] = {150, 110, 0};
    cap_role = F_SMALL;
    CAP = 0;
    R = 0;
    for (int tier = 0; tier < 3; tier++) {
      cap_role = ROLES[tier];
      CAP = measure_cap(cap_role);
      if (rows == 1) {
        R = solve_single(CAP);
        dy = 0;
      } else {
        caps_above = false;
        R = solve_rows(CAP, 1, dy);  // below-first: no top caption band
        int d = (W - 2 * R) / (zone_count - 1);
        if (top_caption_w(cap_role) / 2 > d - R - MINSP / 2) {
          caps_above = true;  // too wide for the corridor: move above
          R = solve_rows(CAP, 2, dy);
        }
      }
      if (R >= RMIN[tier])
        break;
    }
  }
  // centers from the final radius, equal gaps
  int cxs[MAX_ZONES];
  if (rows == 1) {
    int s = (W - zone_count * 2 * R) / (zone_count + 1);
    for (int i = 0; i < zone_count; i++)
      cxs[i] = EDGE + s * (i + 1) + R * (2 * i + 1);
  } else {
    int lo = EDGE + R, hi = 800 - EDGE_R - R;
    int c[MAX_ZONES];
    for (int j = 0; j < zone_count; j++)
      c[j] = lo + (hi - lo) * j / (zone_count - 1);
    if (zone_count == 4) {
      cxs[0] = c[0]; cxs[1] = c[2];               // top: columns 1 and 3
      cxs[2] = c[1]; cxs[3] = c[3];               // bottom: columns 2 and 4
    } else {
      cxs[0] = c[1]; cxs[1] = c[3];               // top pair in the gaps
      cxs[2] = c[0]; cxs[3] = c[2]; cxs[4] = c[4];  // bottom trio
    }
  }
  // vertical placement: single row centers its block; two rows center the
  // overlapped span (a top caption band exists only in caps_above mode)
  int cy_row[2];
  if (rows == 1) {
    cy_row[0] = TOP + (avail - (2 * R + GAP + CAP)) / 2 + R;
  } else {
    int span = 2 * R + dy + (caps_above ? 2 : 1) * (GAP + CAP);
    cy_row[0] = TOP + (avail - span) / 2 + (caps_above ? CAP + GAP : 0) + R;
    cy_row[1] = cy_row[0] + dy;
  }
  bool has_home = home_zone >= 0 && home_zone < zone_count;
  int base_off = has_home ? zone_offset_min(zones[home_zone], now) : 0;
  for (int i = 0; i < zone_count; i++) {
    const Zone &z = zones[i];
    int off = zone_offset_min(z, now);
    time_t local = now + (time_t) off * 60;
    struct tm lt;
    gmtime_r(&local, &lt);
    int row = i < r1 || rows == 1 ? 0 : 1;
    // RTL flips the whole layout horizontally: zone order runs right to
    // left and the staircase mirrors (CECE/ECEC -> ECEC/CECE), margins
    // included (the battery hairline sits left in RTL)
    int cx = mx(cxs[i]);
    int cy = cy_row[row];
    bool is_local = i == home_zone;
    // inverted dial = that zone is inside its configured night window
    bool night = night_mode && is_night(lt.tm_hour);
    const uint8_t *photo = photos ? zone_image(z, R) : nullptr;
    auto fg = night ? esphome::display::COLOR_OFF : esphome::display::COLOR_ON;
    if (night) {
      it.filled_circle(cx, cy, R);
    } else {
      it.circle(cx, cy, R);
      it.circle(cx, cy, R - 1);
    }
    if (photo != nullptr) {
      // day: ink portrait on paper; night: the same face as a negative on
      // the filled dial, matching the paper hands
      int stride = (2 * R + 7) / 8;
      for (int yy = 0; yy < 2 * R; yy++) {
        int py = yy - R;
        for (int xx = 0; xx < 2 * R; xx++) {
          int px = xx - R;
          if (px * px + py * py > (R - 2) * (R - 2))
            continue;
          if (photo[yy * stride + (xx >> 3)] & (0x80 >> (xx & 7)))
            it.draw_pixel_at(cx - R + xx, cy - R + yy, fg);
        }
      }
    }
    // photo faces can be dark (or light, at night) exactly where a hand
    // lies, so every stroke over one first clears a 2 px halo in the
    // opposite color — the boundary keeps full contrast whatever the
    // dither underneath does
    auto halo = night ? esphome::display::COLOR_ON : esphome::display::COLOR_OFF;
    auto stroke = [&](int x0, int y0, int x1, int y1, int w) {
      if (photo != nullptr)
        thick_line(it, x0, y0, x1, y1, w + 4, halo);
      thick_line(it, x0, y0, x1, y1, w, fg);
    };
    for (int k = 0; k < 12; k++) {
      float a = k * PI_F / 6.0f;
      bool quarter = (k % 3 == 0);  // longer, heavier markers at 12/3/6/9
      int inner = quarter ? R - 20 : R - 12;
      stroke(cx + (int) (sinf(a) * inner), cy - (int) (cosf(a) * inner),
             cx + (int) (sinf(a) * (R - 4)), cy - (int) (cosf(a) * (R - 4)),
             quarter ? 5 : 2);
    }
    float ah = ((lt.tm_hour % 12) + lt.tm_min / 60.0f) * PI_F / 6.0f;
    float am = lt.tm_min * PI_F / 30.0f;
    stroke(cx, cy, cx + (int) (sinf(ah) * R * 0.52f), cy - (int) (cosf(ah) * R * 0.52f), 6);
    stroke(cx, cy, cx + (int) (sinf(am) * R * 0.85f), cy - (int) (cosf(am) * R * 0.85f), 4);
    if (photo != nullptr)
      it.filled_circle(cx, cy, 7, halo);
    it.filled_circle(cx, cy, 5, fg);
    // the local zone is marked by an inverted caption instead of its dial
    char label[48];
    const char *cname = lang::zone_name(z);
    BaseFont *cap_f = label_font(cap_role, cname);
    if (show_offsets && !is_local) {
      int rel = off - base_off;  // vs. home, or vs. UTC with no home
      int a2 = rel < 0 ? -rel : rel;
      snprintf(label, sizeof(label), "%s %c%d:%02d", cname, rel < 0 ? '-' : '+', a2 / 60,
               a2 % 60);
    } else {
      snprintf(label, sizeof(label), "%s", cname);
    }
    if (labels_inside) {
      // lower half of the face, colors following the dial's polarity
      int ly = cy + (int) (R * 0.38f);
      auto tcol = night ? esphome::display::COLOR_ON : esphome::display::COLOR_OFF;
      if (is_local) {
        int bx, by, bw, bh;
        it.get_text_bounds(cx, ly, label, cap_f, TextAlign::TOP_CENTER, &bx, &by, &bw, &bh);
        it.filled_rectangle(bx - 6, by - 3, bw + 12, bh + 6, fg);
        it.print(cx, ly, cap_f, tcol, TextAlign::TOP_CENTER, label);
      } else if (photo != nullptr) {
        // same contrast rule as the hands: clear a chip under the label
        int bx, by, bw, bh;
        it.get_text_bounds(cx, ly, label, cap_f, TextAlign::TOP_CENTER, &bx, &by, &bw, &bh);
        it.filled_rectangle(bx - 6, by - 3, bw + 12, bh + 6, halo);
        it.print(cx, ly, cap_f, fg, TextAlign::TOP_CENTER, label);
      } else {
        it.print(cx, ly, cap_f, fg, TextAlign::TOP_CENTER, label);
      }
    } else {
      // top-row captions go above only when the engine had to move them
      // (corridor between the rising bottom dials too narrow)
      bool above = rows == 2 && row == 0 && caps_above;
      int ly = above ? cy - R - GAP - CAP + 4 : cy + R + GAP;
      if (is_local) {
        int bx, by, bw, bh;
        it.get_text_bounds(cx, ly, label, cap_f, TextAlign::TOP_CENTER, &bx, &by, &bw, &bh);
        it.filled_rectangle(bx - 6, by - 3, bw + 12, bh + 6);
        it.print(cx, ly, cap_f, esphome::display::COLOR_OFF, TextAlign::TOP_CENTER, label);
      } else {
        it.print(cx, ly, cap_f, TextAlign::TOP_CENTER, label);
      }
    }
  }
}

// Calendar page: today in all three calendars plus the zodiac sign, big
// type, no time — nothing on it changes within a day, so the sleep model
// stretches to local midnight while it is shown. Fully localized: names,
// digits, and word order follow the language pack.
inline void draw_calendar(Display &it, BaseFont *zodiac_f) {
  time_t now = ::time(nullptr);
  BaseFont *med = lfont(F_MEDIUM);
  if (now < MIN_VALID_EPOCH) {
    it.print(400, 220, med, TextAlign::CENTER, lang::L().wait_sync);
    return;
  }
  struct tm lt;
  time_t local = home_local(now);
  gmtime_r(&local, &lt);
  int jy, jm, jd, hy, hm, hd;
  jalali::from_gregorian(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, jy, jm, jd);
  bool exact = hijri::from_gregorian(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, hy, hm, hd);
  // dates take the reading-side two thirds, the zodiac the other third
  int dx = lang::rtl() ? 533 : 267;
  int zx = lang::rtl() ? 133 : 666;
  it.print(dx, 24, lfont(F_LARGE), TextAlign::TOP_CENTER, lang::L().wdays_full[lt.tm_wday]);
  // Jalali carries the page; Gregorian and Lunar Hijri follow. Day, month
  // and year render as aligned columns sized from today's actual strings —
  // no constant gaps, so Shahrivar costs Tir nothing.
  struct {
    char d[16], m[64], y[16];
  } row[3];
  char tmp[16];
  auto fill = [&](int i, int day, const char *mon, int year) {
    snprintf(tmp, sizeof(tmp), "%d", day);
    lang::num(row[i].d, sizeof(row[i].d), tmp);
    snprintf(row[i].m, sizeof(row[i].m), "%s", mon);
    snprintf(tmp, sizeof(tmp), "%d", year);
    lang::num(row[i].y, sizeof(row[i].y), tmp);
  };
  fill(0, jd, lang::jalali_month(jm - 1), jy);
  fill(1, lt.tm_mday, lang::L().months_full[lt.tm_mon], lt.tm_year + 1900);
  fill(2, hd, lang::hijri_month(hm - 1), hy);
  BaseFont *tf = lfont(F_TIME);
  int wd = 0, wm = 0, wy = 0;
  for (int i = 0; i < 3; i++) {
    int bx, by, bw, bh;
    it.get_text_bounds(0, 0, row[i].d, tf, TextAlign::TOP_LEFT, &bx, &by, &bw, &bh);
    wd = bw > wd ? bw : wd;
    it.get_text_bounds(0, 0, row[i].m, tf, TextAlign::TOP_LEFT, &bx, &by, &bw, &bh);
    wm = bw > wm ? bw : wm;
    it.get_text_bounds(0, 0, row[i].y, tf, TextAlign::TOP_LEFT, &bx, &by, &bw, &bh);
    wy = bw > wy ? bw : wy;
  }
  const int G = 28;
  int total = wd + G + wm + G + wy;
  int x0 = dx - total / 2;
  static const int ROW_Y[3] = {120, 204, 288};
  for (int i = 0; i < 3; i++) {
    int y = ROW_Y[i];
    if (!lang::rtl()) {  // day right-aligned (units line up), the rest left
      it.print(x0 + wd, y, tf, TextAlign::TOP_RIGHT, row[i].d);
      it.print(x0 + wd + G, y, tf, TextAlign::TOP_LEFT, row[i].m);
      it.print(x0 + wd + G + wm + G, y, tf, TextAlign::TOP_LEFT, row[i].y);
    } else {  // mirrored: day rightmost, columns grow leftward
      it.print(x0 + total, y, tf, TextAlign::TOP_RIGHT, row[i].d);
      it.print(x0 + total - wd - G, y, tf, TextAlign::TOP_RIGHT, row[i].m);
      it.print(x0 + wy, y, tf, TextAlign::TOP_RIGHT, row[i].y);
    }
  }
  // which lunar calendar produced that date: the exact Iranian table or
  // the arithmetic fallback outside its window
  it.print(dx, 366, lfont(F_TINY), TextAlign::TOP_CENTER,
           exact ? lang::L().cal_ir : lang::L().cal_tab);
  it.line(lang::rtl() ? 267 : 533, 40, lang::rtl() ? 267 : 533, 410);
  // the sign is the sun's position — one sign per day, every calendar;
  // the symbol is universal (and the column's anchor), the name localized
  static const char *const SIGNS[12] = {"♈", "♉", "♊", "♋", "♌", "♍",
                                        "♎", "♏", "♐", "♑", "♒", "♓"};
  it.print(zx, 60, lfont(F_SMALL), TextAlign::TOP_CENTER, lang::L().zodiac_label);
  it.print(zx, 210, zodiac_f, TextAlign::CENTER, SIGNS[jm - 1]);
  it.print(zx, 316, med, TextAlign::TOP_CENTER, lang::L().zodiac[jm - 1]);
}

// Hardware/software/config snapshot, double green press. Deliberately LTR
// and English in every language: it is a technical readout.
inline void draw_debug(Display &it, float batt_v, float batt_pct, bool sd_present,
                       const char *cfg_url) {
  char title[48];
  snprintf(title, sizeof(title), "Debug - %d min left", debug_minutes_left());
  it.print(400, 10, lfont(F_MEDIUM), TextAlign::TOP_CENTER, title);
  it.line(20, 52, 780, 52);
  char rows[16][2][64];
  int n = 0;
  char v[64];
  auto put = [&](const char *k, const char *val) {
    snprintf(rows[n][0], sizeof(rows[n][0]), "%s", k);
    snprintf(rows[n][1], sizeof(rows[n][1]), "%s", val);
    n++;
  };
  // the name its HA helpers carry (make helpers UNIT=...)
  put("Unit", esphome::App.get_name().c_str());
#ifdef ESPHOME_PROJECT_VERSION
  put("Firmware", ESPHOME_PROJECT_VERSION);
#else
  put("Firmware", "dev (no version stamp)");
#endif
  put("ESPHome", ESPHOME_VERSION);
  put("ESP-IDF", esp_get_idf_version());
  esp_chip_info_t ci;
  esp_chip_info(&ci);
  snprintf(v, sizeof(v), "ESP32-S3 rev %d, %d cores", (int) ci.revision, (int) ci.cores);
  put("Chip", v);
  uint32_t fs = 0;
  esp_flash_get_size(nullptr, &fs);
  snprintf(v, sizeof(v), "%u MB flash, %u KB PSRAM free", (unsigned) (fs >> 20),
           (unsigned) (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  put("Memory", v);
  snprintf(v, sizeof(v), "%u KB free, %u KB largest block",
           (unsigned) (esp_get_free_heap_size() / 1024),
           (unsigned) (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024));
  put("Heap", v);
  auto *w = esphome::wifi::global_wifi_component;
  if (wifi_strikeout()) {
    put("Wi-Fi", "gave up (3 failed wakes)");
  } else if (w->is_connected()) {
    char ssid[esphome::wifi::SSID_BUFFER_SIZE];
    w->wifi_ssid_to(std::span<char, esphome::wifi::SSID_BUFFER_SIZE>(ssid));
    char ip[esphome::network::IP_ADDRESS_BUFFER_SIZE];
    w->wifi_sta_ip_addresses()[0].str_to(ip);
    snprintf(v, sizeof(v), "%s  %s  %d dBm", ssid, ip, (int) w->wifi_rssi());
    put("Wi-Fi", v);
  } else {
    put("Wi-Fi", radio_on ? "not connected" : "radio off");
  }
  snprintf(v, sizeof(v), "%.2f V  %.0f%%", (double) batt_v, (double) batt_pct);
  put("Battery", v);
  put("RTC", rtc_bad ? "BAD" : "ok");
  put("SD card", sd_present ? "present" : "none");
  snprintf(v, sizeof(v), "%u s awake, %d wifi strikes", (unsigned) (millis() / 1000),
           wifi_fail_wakes);
  put("Uptime", v);
  if (night_refresh_min > 0)
    snprintf(v, sizeof(v), "refresh %d min (night %d), sync %d min", refresh_interval_min,
             night_refresh_min, sync_interval_min);
  else
    snprintf(v, sizeof(v), "refresh %d min, sync %d min", refresh_interval_min,
             sync_interval_min);
  put("Intervals", v);
  snprintf(v, sizeof(v), "%d zones, home %s, lang %s", zone_count,
           home_zone >= 0 ? zones[home_zone].labels[0] : "none", lang::L().code);
  put("Zones", v);
  if (cfg_url[0] != 0) {
    time_t now = ::time(nullptr);
    if (cfg_fetch_at > 0 && now > cfg_fetch_at)
      snprintf(v, sizeof(v), "%s (checked %ldh ago)", cfg_url, (long) ((now - cfg_fetch_at) / 3600));
    else
      snprintf(v, sizeof(v), "%s", cfg_url);
    put("Config URL", v);
  } else {
    put("Config URL", "unset");
  }
  BaseFont *lab = lfont(F_SMALLB), *val = lfont(F_SMALL);
  int y = 68;
  for (int i = 0; i < n; i++, y += 25) {
    it.print(40, y, lab, TextAlign::TOP_LEFT, rows[i][0]);
    it.print(230, y, val, TextAlign::TOP_LEFT, rows[i][1]);
  }
}

// Empty-battery screen: the frame a dead device is left holding. Drawn as
// primitives rather than a glyph — the UI fonts are subset to the strings
// they render and carry no battery symbol. The clock line answers the
// question the frozen panel raises: how long has it been dead?
inline void draw_charge(Display &it) {
  const int W = 300, T = 8;  // T = outline weight
  // the glyph, the word and the time share the content row from a third of
  // the way down, so a taller status bar lifts the whole block
  int bottom = content_bottom(CHARGE_PAGE);
  int Y = bottom / 4, H = bottom * 5 / 16, X = 400 - W / 2;
  for (int i = 0; i < T; i++)
    it.rectangle(X + i, Y + i, W - 2 * i, H - 2 * i);
  int nub_x = lang::rtl() ? X - 20 : X + W;  // terminal on the reading-end side
  it.filled_rectangle(nub_x, Y + H / 2 - 25, 20, 50);
  it.print(400, Y + H + 60, lfont(F_LARGE), TextAlign::TOP_CENTER, lang::L().charge);
  time_t now = ::time(nullptr);
  if (now > MIN_VALID_EPOCH) {
    struct tm lt;
    char buf[32], loc[48];
    localtime_r(&now, &lt);
    strftime(buf, sizeof(buf), "%H:%M", &lt);
    lang::num(loc, sizeof(loc), buf);
    it.print(400, Y + H + 130, lfont(F_MEDIUM), TextAlign::TOP_CENTER, loc);
  }
}

// Random quatrain, re-picked on first use and then every few hours.
RTC_DATA_ATTR int khayyam_idx = -1;
RTC_DATA_ATTR time_t khayyam_at = 0;
constexpr time_t KHAYYAM_INTERVAL = 6 * 3600;

// Green button on the Khayyam page: force a fresh pick at the next redraw.
inline void khayyam_reroll() { khayyam_idx = -1; }

inline void draw_khayyam(Display &it, BaseFont *title_f, BaseFont *body_f) {
  time_t now = ::time(nullptr);
  if (khayyam_idx < 0 || khayyam_idx >= KHAYYAM_COUNT ||
      (now > MIN_VALID_EPOCH && now - khayyam_at > KHAYYAM_INTERVAL)) {
    khayyam_idx = (int) (esp_random() % (uint32_t) KHAYYAM_COUNT);
    khayyam_at = now;
  }
  it.print(400, 28, title_f, TextAlign::TOP_CENTER, KHAYYAM_TITLE);
  it.line(240, 118, 560, 118);
  const Quatrain &q = KHAYYAM_FA[khayyam_idx];
  const int TOP = 158, LINE_H = 46;  // body face plus its descender room
  // the three gaps between the four lines share what the content row leaves
  // under the title, never stretching past the designed leading
  int step = std::min(76, (content_bottom(page) - TOP - LINE_H) / 3);
  int y = TOP;
  for (int i = 0; i < 4; i++) {
    it.print(400, y, body_f, TextAlign::TOP_CENTER, q.l[i]);
    y += step;
  }
}

}  // namespace reterminal
