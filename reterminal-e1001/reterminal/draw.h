#pragma once

// Page renderers for the 800x480 ePaper.

#include "esphome/components/display/display.h"
#include "esp_system.h"
#include "device.h"
#include "khayyam_fa.h"
#include "hijri.h"
#include "jalali.h"
#include "pure.h"

namespace reterminal {

using esphome::display::BaseFont;
using esphome::display::Display;
using esphome::display::TextAlign;

// --- Pages ----------------------------------------------------------------

// Small sun (day) / crescent moon (night) glyph; bg carves the crescent so
// it works on normal and inverted rows alike.
inline void thick_line(Display &it, int x1, int y1, int x2, int y2, int w, esphome::Color color);

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

inline void draw_world_clock(Display &it, BaseFont *time_f, BaseFont *med, BaseFont *small_f) {
  time_t now = ::time(nullptr);
  if (now < MIN_VALID_EPOCH) {
    it.print(400, 220, med, TextAlign::CENTER, "Waiting for time sync...");
    return;
  }
  if (zone_count == 0) {
    it.print(400, 220, med, TextAlign::CENTER, "No zones configured");
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
    char hhmm[8], date[16];
    strftime(hhmm, sizeof(hhmm), "%H:%M", &lt);
    strftime(date, sizeof(date), "%a %d %b", &lt);
    bool is_local = zi == home_zone;
    auto fg = is_local ? esphome::display::COLOR_OFF : esphome::display::COLOR_ON;
    int cy = y + RH / 2;
    if (is_local)
      it.filled_rectangle(0, y, 800, RH);
    it.print(40, cy, med, fg, TextAlign::CENTER_LEFT, z.city);
    if (!is_local) {
      int rel = off - base_off;
      int a = rel < 0 ? -rel : rel;
      it.printf(430, cy + 4, small_f, fg, TextAlign::CENTER_RIGHT, "%c%d:%02d",
                rel < 0 ? '-' : '+', a / 60, a % 60);
    }
    it.print(580, cy + 4, small_f, fg, TextAlign::CENTER_RIGHT, date);
    it.print(726, cy, time_f, fg, TextAlign::CENTER_RIGHT, hhmm);
    draw_day_night(it, 762, cy, is_night(lt.tm_hour), fg,
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
inline void draw_status_bar(Display &it, BaseFont *small_f, float battery_pct, float dev_t,
                            float dev_h, bool show_climate, bool sd_present, bool show_jalali,
                            bool show_lunar, bool awake_hold) {
  it.filled_rectangle(0, 446, 800, 34);
  time_t now = ::time(nullptr);
  if (now > MIN_VALID_EPOCH) {
    struct tm lt;
    localtime_r(&now, &lt);
    char greg[24];
    strftime(greg, sizeof(greg), "%a %d %b %Y", &lt);
    int lx = 20;
    if (show_jalali) {
      int jy, jm, jd;
      jalali::from_gregorian(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, jy, jm, jd);
      char buf[28];
      snprintf(buf, sizeof(buf), "%d %s %d", jd, jalali::MONTHS[jm - 1], jy);
      it.print(lx, 463, small_f, esphome::display::COLOR_OFF, TextAlign::CENTER_LEFT, buf);
      int bx, by, bw, bh;
      it.get_text_bounds(lx, 463, buf, small_f, TextAlign::CENTER_LEFT, &bx, &by, &bw, &bh);
      lx += bw + 24;
    }
    if (show_lunar) {
      int hy, hm, hd;
      hijri::from_gregorian(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, hy, hm, hd);
      it.printf(lx, 463, small_f, esphome::display::COLOR_OFF, TextAlign::CENTER_LEFT,
                "%d %s %d", hd, hijri::MONTHS[hm - 1], hy);
    }
    it.print(400, 463, small_f, esphome::display::COLOR_OFF, TextAlign::CENTER, greg);
  }
  int x = 780;
  if (!std::isnan(battery_pct)) {
    it.printf(x, 463, small_f, esphome::display::COLOR_OFF, TextAlign::CENTER_RIGHT, "%.0f%%",
              battery_pct);
    x -= 58;
  }
  if (awake_hold) {
    draw_awake_icon(it, x - 11, 451);
    x -= 24;
  }
  if (sd_present) {
    draw_sd_icon(it, x - 10, 456);
    x -= 22;
  }
  if (rtc_bad) {
    it.print(x, 463, small_f, esphome::display::COLOR_OFF, TextAlign::CENTER_RIGHT, "RTC!");
    x -= 54;
  }
  if (show_climate && !std::isnan(dev_t) && !std::isnan(dev_h))
    it.printf(x, 463, small_f, esphome::display::COLOR_OFF, TextAlign::CENTER_RIGHT,
              "T %.0f° H %.0f", dev_t, dev_h);
}

inline void draw_numbers(Display &it, BaseFont *huge, BaseFont *large, BaseFont *med,
                         BaseFont *small_f) {
  int en[MAX_COLS], n = 0;
  for (int i = 0; i < MAX_COLS; i++)
    if (cols[i].enabled)
      en[n++] = i;
  if (n == 0) {
    it.print(400, 220, med, TextAlign::CENTER, "No columns configured");
    return;
  }
  static const int CX[3][3] = {{400, 0, 0}, {200, 600, 0}, {133, 400, 667}};
  if (n >= 2)
    it.line(n == 2 ? 400 : 267, 50, n == 2 ? 400 : 267, 400);
  if (n == 3)
    it.line(533, 50, 533, 400);
  for (int k = 0; k < n; k++) {
    const Column &c = cols[en[k]];
    int x = CX[n - 1][k];
    float t = last_val[en[k]][M_TEMP], h = last_val[en[k]][M_HUM];
    it.print(x, 40, med, TextAlign::TOP_CENTER, c.label);
    // the value is the information, the unit a caption on its shoulder
    char buf[16];
    int bx, by, bw, bh;
    if (std::isnan(t)) {
      it.print(x, 140, large, TextAlign::TOP_CENTER, "--");
    } else {
      snprintf(buf, sizeof(buf), "%.1f", t);
      it.print(x, 110, huge, TextAlign::TOP_CENTER, buf);
      it.get_text_bounds(x, 110, buf, huge, TextAlign::TOP_CENTER, &bx, &by, &bw, &bh);
      it.print(bx + bw + 6, 122, small_f, TextAlign::TOP_LEFT, "°C");
    }
    if (std::isnan(h)) {
      it.print(x, 290, large, TextAlign::TOP_CENTER, "--");
    } else {
      snprintf(buf, sizeof(buf), "%.0f", h);
      it.print(x, 270, huge, TextAlign::TOP_CENTER, buf);
      it.get_text_bounds(x, 270, buf, huge, TextAlign::TOP_CENTER, &bx, &by, &bw, &bh);
      it.print(bx + bw + 6, 282, small_f, TextAlign::TOP_LEFT, "%");
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

inline bool no_cols(Display &it, BaseFont *med) {
  for (int i = 0; i < MAX_COLS; i++)
    if (cols[i].enabled)
      return false;
  it.print(400, 220, med, TextAlign::CENTER, "No columns configured");
  return true;
}

inline void draw_graph_page(Display &it, int metric, const char *title, BaseFont *med,
                            BaseFont *small_f, BaseFont *tiny_f) {
  it.print(20, 4, med, TextAlign::TOP_LEFT, title);
  if (no_cols(it, med))
    return;
  draw_legend(it, tiny_f, 784, 10);
  draw_graph_band(it, series_day, metric, fetch_time, SPAN, med, tiny_f, 48, 420, true);
}

// Combined page over week_days: temperature and humidity stacked, one
// legend, shared day axis at the bottom, vertical inverted section titles.
inline void draw_week_page(Display &it, BaseFont *med, BaseFont *small_f, BaseFont *tiny_f) {
  if (no_cols(it, med))
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
inline void draw_analog_clocks(Display &it, BaseFont *med, BaseFont *small_f,
                               bool show_offsets) {
  time_t now = ::time(nullptr);
  if (now < MIN_VALID_EPOCH) {
    it.print(400, 220, med, TextAlign::CENTER, "Waiting for time sync...");
    return;
  }
  constexpr float PI_F = 3.14159265f;
  // Dial positions per zone count (1..5), full-size dials throughout.
  // Two-row layouts interleave: the bottom row sits in the top row's gaps.
  // 5 clocks: C.C.C over .C.C.; 4 clocks is the same minus the top-right
  // dial, keeping the interlocked cluster. Captions in the 16 px utility
  // face buy the 8-12 px bezel margins the panel edge needs.
  static const struct {
    int cx, cy;
  } LAYOUTS[MAX_ZONES][MAX_ZONES] = {
      {{400, 220}},
      {{266, 220}, {533, 220}},
      {{133, 220}, {400, 220}, {667, 220}},
      {{108, 104}, {400, 104}, {254, 323}, {546, 323}},
      {{108, 104}, {400, 104}, {692, 104}, {254, 323}, {546, 323}},
  };
  const int R = 96;
  if (zone_count == 0) {
    it.print(400, 220, med, TextAlign::CENTER, "No zones configured");
    return;
  }
  const auto *pos = LAYOUTS[zone_count - 1];
  bool has_home = home_zone >= 0 && home_zone < zone_count;
  int base_off = has_home ? zone_offset_min(zones[home_zone], now) : 0;
  for (int i = 0; i < zone_count; i++) {
    const Zone &z = zones[i];
    int off = zone_offset_min(z, now);
    time_t local = now + (time_t) off * 60;
    struct tm lt;
    gmtime_r(&local, &lt);
    int cx = pos[i].cx, cy = pos[i].cy;
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
    char label[32];
    if (show_offsets && !is_local) {
      int rel = off - base_off;  // vs. home, or vs. UTC with no home
      int a2 = rel < 0 ? -rel : rel;
      snprintf(label, sizeof(label), "%s %c%d:%02d", z.city, rel < 0 ? '-' : '+', a2 / 60,
               a2 % 60);
    } else {
      snprintf(label, sizeof(label), "%s", z.city);
    }
    int ly = cy + R + (zone_count >= 4 ? 4 : 10);
    if (is_local) {
      int bx, by, bw, bh;
      it.get_text_bounds(cx, ly, label, small_f, TextAlign::TOP_CENTER, &bx, &by, &bw, &bh);
      it.filled_rectangle(bx - 6, by - 3, bw + 12, bh + 6);
      it.print(cx, ly, small_f, esphome::display::COLOR_OFF, TextAlign::TOP_CENTER, label);
    } else {
      it.print(cx, ly, small_f, TextAlign::TOP_CENTER, label);
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
