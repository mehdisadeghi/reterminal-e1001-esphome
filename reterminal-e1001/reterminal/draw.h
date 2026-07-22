#pragma once

// Page renderers for the 800x480 ePaper.

#include <algorithm>

#include "esphome/components/display/display.h"
#include "esp_system.h"
#include "device.h"
#include "khayyam_fa.h"
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
  const int RH = 76;
  int y = 58 + (MAX_ZONES - zone_count) * (RH / 2);  // vertically centered
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

inline void draw_sd_icon(Display &it, int x, int y) {
  auto c = esphome::display::COLOR_OFF;
  it.line(x + 3, y, x + 9, y, c);  // top edge, shortened by the corner notch
  it.line(x + 9, y, x + 9, y + 13, c);
  it.line(x, y + 13, x + 9, y + 13, c);
  it.line(x, y + 3, x, y + 13, c);
  it.line(x, y + 3, x + 3, y, c);
  for (int px = x + 2; px <= x + 8; px += 2)
    it.line(px, y + 2, px, y + 4, c);
}

// Lit bulb = the device is held awake (Pause Deep Sleep or keep-awake helper)
inline void draw_awake_icon(Display &it, int x, int y) {
  auto c = esphome::display::COLOR_OFF;
  it.line(x + 5, y, x + 5, y + 1, c);  // rays
  it.line(x, y + 2, x + 1, y + 3, c);
  it.line(x + 9, y + 3, x + 10, y + 2, c);
  it.circle(x + 5, y + 8, 4, c);  // glass
  it.line(x + 3, y + 14, x + 7, y + 14, c);  // base
  it.line(x + 3, y + 16, x + 7, y + 16, c);
}

// Bottom status bar, inverted. Left: Solar Hijri and/or Lunar Hijri date;
// center: Gregorian date; right, growing leftwards: battery, awake bulb,
// SD-card icon, RTC warning, device climate.
inline void draw_status_bar(Display &it, float battery_pct, float dev_t, float dev_h,
                            bool show_climate, bool sd_present, bool show_jalali,
                            bool show_lunar, bool awake_hold) {
  BaseFont *small_f = lfont(F_SMALLB);
  it.filled_rectangle(0, 446, 800, 34);
  time_t now = ::time(nullptr);
  // shaped Persian dates run ~60 bytes (presentation forms are 3-byte UTF-8)
  char buf[96];
  auto astart = ma(TextAlign::CENTER_LEFT);
  auto aend = ma(TextAlign::CENTER_RIGHT);
  int dir = lang::rtl() ? -1 : 1;
  if (now > MIN_VALID_EPOCH) {
    struct tm lt;
    localtime_r(&now, &lt);
    int lx = mx(20);
    if (show_jalali) {
      int jy, jm, jd;
      jalali::from_gregorian(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, jy, jm, jd);
      lang::d_m_y(buf, sizeof(buf), jd, lang::jalali_month(jm - 1), jy);
      it.print(lx, 463, small_f, esphome::display::COLOR_OFF, astart, buf);
      int bx, by, bw, bh;
      it.get_text_bounds(lx, 463, buf, small_f, astart, &bx, &by, &bw, &bh);
      lx += dir * (bw + 24);
    }
    if (show_lunar) {
      int hy, hm, hd;
      hijri::from_gregorian(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, hy, hm, hd);
      lang::d_m_y(buf, sizeof(buf), hd, lang::hijri_month(hm - 1), hy);
      it.print(lx, 463, small_f, esphome::display::COLOR_OFF, astart, buf);
    }
    lang::date_long(buf, sizeof(buf), lt.tm_wday, lt.tm_mday, lt.tm_mon, lt.tm_year + 1900);
    it.print(400, 463, small_f, esphome::display::COLOR_OFF, TextAlign::CENTER, buf);
  }
  int x = mx(780);
  if (!std::isnan(battery_pct)) {
    char pct[8];
    snprintf(pct, sizeof(pct), "%.0f", battery_pct);
    lang::percent(buf, sizeof(buf), pct);
    it.print(x, 463, small_f, esphome::display::COLOR_OFF, aend, buf);
    x -= dir * 58;
  }
  if (awake_hold) {
    draw_awake_icon(it, lang::rtl() ? x : x - 11, 451);
    x -= dir * 24;
  }
  if (sd_present) {
    draw_sd_icon(it, lang::rtl() ? x : x - 10, 456);
    x -= dir * 22;
  }
  if (rtc_bad) {
    it.print(x, 463, small_f, esphome::display::COLOR_OFF, aend, "RTC!");
    x -= dir * 54;
  }
  if (show_climate && !std::isnan(dev_t) && !std::isnan(dev_h))
    it.printf(x, 463, small_f, esphome::display::COLOR_OFF, aend,
              "T %.0f° H %.0f", dev_t, dev_h);
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
  draw_graph_band(it, series_day, metric, fetch_time, SPAN, med, tiny_f, 48, 420, true);
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

  for (const auto &band : BANDS) {
    it.filled_rectangle(0, band.y0, 32, band.y1 - band.y0);
    int nt = (int) strlen(band.tag);
    int ty = (band.y0 + band.y1) / 2 - nt * 15;
    for (int i = 0; i < nt; i++) {
      char ch[2] = {band.tag[i], 0};
      it.print(16, ty + i * 30, small_f, esphome::display::COLOR_OFF, TextAlign::TOP_CENTER, ch);
    }
    draw_graph_band(it, series_week, band.metric, week_fetch_time, week_span(), med, tiny_f,
                    band.y0, band.y1, false);
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
      it.print(x, 424, tiny_f, TextAlign::TOP_CENTER, day);
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
inline void draw_analog_clocks(Display &it, bool show_offsets, bool labels_inside) {
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
  int avail = (bar_on_[page] ? 446 : 474) - TOP;
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
  // columns, not stacked row blocks. Top-row captions go above the dials,
  // bottom-row below, keeping text out of the interleave zone. dy_out is
  // the solved distance between the two rows' centers.
  int dy = 0;
  auto solve_r = [&](int cap, int &dy_out) {
    dy_out = 0;
    if (rows == 1) {
      int r = (avail - GAP - cap) / 2;
      return std::min(r, (W - (zone_count + 1) * MINSP) / (2 * zone_count));
    }
    for (int r = avail / 2; r >= 20; r--) {
      int d = (W - 2 * r) / (zone_count - 1);
      if (d <= 0)
        continue;
      long need = 2L * r + MINSP;
      long dd = (long) d * d;
      long dy2 = need * need - dd;
      int v = dy2 > 0 ? (int) ceilf(sqrtf((float) dy2)) : 0;
      if (2 * r + v + 2 * (GAP + cap) <= avail) {
        dy_out = v;
        return r;
      }
    }
    return 20;
  };

  int cap_role, CAP, R;
  if (labels_inside) {
    CAP = 0;
    R = solve_r(0, dy);
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
      R = solve_r(CAP, dy);
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
  // overlapped span (top captions above, bottom captions below)
  int cy_row[2];
  if (rows == 1) {
    cy_row[0] = TOP + (avail - (2 * R + GAP + CAP)) / 2 + R;
  } else {
    int span = 2 * R + dy + 2 * (GAP + CAP);
    cy_row[0] = TOP + (avail - span) / 2 + CAP + GAP + R;
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
    int cx = cxs[i];
    int cy = cy_row[row];
    bool is_local = i == home_zone;
    // inverted dial = that zone is inside its configured night window
    bool night = is_night(lt.tm_hour);
    auto fg = night ? esphome::display::COLOR_OFF : esphome::display::COLOR_ON;
    if (night) {
      it.filled_circle(cx, cy, R);
    } else {
      it.circle(cx, cy, R);
      it.circle(cx, cy, R - 1);
    }
    for (int k = 0; k < 12; k++) {
      float a = k * PI_F / 6.0f;
      bool quarter = (k % 3 == 0);  // longer, heavier markers at 12/3/6/9
      int inner = quarter ? R - 20 : R - 12;
      thick_line(it, cx + (int) (sinf(a) * inner), cy - (int) (cosf(a) * inner),
                 cx + (int) (sinf(a) * (R - 4)), cy - (int) (cosf(a) * (R - 4)),
                 quarter ? 5 : 2, fg);
    }
    float ah = ((lt.tm_hour % 12) + lt.tm_min / 60.0f) * PI_F / 6.0f;
    float am = lt.tm_min * PI_F / 30.0f;
    thick_line(it, cx, cy, cx + (int) (sinf(ah) * R * 0.52f), cy - (int) (cosf(ah) * R * 0.52f),
               6, fg);
    thick_line(it, cx, cy, cx + (int) (sinf(am) * R * 0.85f), cy - (int) (cosf(am) * R * 0.85f),
               4, fg);
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
      } else {
        it.print(cx, ly, cap_f, fg, TextAlign::TOP_CENTER, label);
      }
    } else {
      // two-row layouts: top-row captions sit above their dials, clear of
      // the interleave zone the bottom dials rise into
      bool above = rows == 2 && row == 0;
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
  int y = 158;
  for (int i = 0; i < 4; i++) {
    it.print(400, y, body_f, TextAlign::TOP_CENTER, q.l[i]);
    y += 76;
  }
}

}  // namespace reterminal
