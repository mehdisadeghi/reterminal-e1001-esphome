#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esphome/components/display/display.h"
#include "esphome/core/log.h"
#include "jalali.h"

namespace reterminal {

using esphome::display::BaseFont;
using esphome::display::Display;
using esphome::display::TextAlign;

static const char *const TAG = "reterminal";

constexpr int PAGE_COUNT = 7;  // world clock, numbers, temp, hum, combined, analog, noqte
constexpr int POINTS = 192;    // 24 h in 7.5 min buckets
constexpr time_t SPAN = 24 * 3600;
constexpr time_t SPAN_WEEK = 7 * 24 * 3600;
constexpr time_t FETCH_INTERVAL = 3 * 3600;
constexpr time_t MIN_VALID_EPOCH = 1600000000;  // clock has been synced at least once

enum Series { S_IN_T = 0, S_OUT_T, S_IN_H, S_OUT_H, SERIES_COUNT };

static const char *const ENTITIES[SERIES_COUNT] = {
    "sensor.indoor_temperature",  // indoor
    "sensor.outdoor_temperature",  // outdoor
    "sensor.indoor_humidity",
    "sensor.outdoor_humidity",
};

// State kept in RTC slow memory: survives deep sleep, cleared on reflash or
// power loss. Graphs repopulate on the next scheduled fetch.
RTC_DATA_ATTR int page = 0;
RTC_DATA_ATTR time_t fetch_time = 0;  // end of the graph window; 0 = no history yet
RTC_DATA_ATTR float series[SERIES_COUNT][POINTS];
RTC_DATA_ATTR time_t week_fetch_time = 0;
RTC_DATA_ATTR float series_week[SERIES_COUNT][POINTS];
RTC_DATA_ATTR float last_val[SERIES_COUNT] = {NAN, NAN, NAN, NAN};
RTC_DATA_ATTR time_t last_val_time = 0;
RTC_DATA_ATTR time_t beep_ack = 0;  // last handled "single beep" request

// Awake bookkeeping (plain RAM, reset each wake). Any command bumps the
// deadline; an interval enters sleep once it passes and no script is running.
static uint32_t sleep_deadline = 0;
static bool ota_in_progress = false;

inline void bump_awake(float wake_s) {
  uint32_t s = (std::isnan(wake_s) || wake_s < 1.0f) ? 60 : (uint32_t) wake_s;
  sleep_deadline = millis() + s * 1000;
}

inline bool sleep_due() { return !ota_in_progress && millis() > sleep_deadline; }

enum WakeAction { WAKE_TIMER, WAKE_PREV, WAKE_NEXT, WAKE_FETCH };

inline WakeAction wake_action() {
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1)
    return WAKE_TIMER;
  uint64_t pins = esp_sleep_get_ext1_wakeup_status();
  if (pins & (1ULL << 5))
    return WAKE_PREV;  // left white
  if (pins & (1ULL << 4))
    return WAKE_NEXT;  // right white
  if (pins & (1ULL << 3))
    return WAKE_FETCH;  // green
  return WAKE_TIMER;
}

inline bool fetch_due() {
  time_t now = ::time(nullptr);
  return now > MIN_VALID_EPOCH && (fetch_time == 0 || now - fetch_time > FETCH_INTERVAL);
}

// Mirrors of HA switches and boot diagnostics, set from YAML.
static bool device_only = false;  // climate pages use the onboard SHT4x only
static bool radio_on = true;      // wifi enabled this wake
static bool rtc_bad = false;      // RTC had no valid time at boot (dead cell / VL)

// In device-only mode the in/out 24 h graph pages are meaningless.
inline bool page_hidden(int p) { return device_only && (p == 2 || p == 3); }

inline void prev_page() {
  do {
    page = (page + PAGE_COUNT - 1) % PAGE_COUNT;
  } while (page_hidden(page));
}
inline void next_page() {
  do {
    page = (page + 1) % PAGE_COUNT;
  } while (page_hidden(page));
}

// --- Wake economics ---------------------------------------------------------

RTC_DATA_ATTR time_t time_sync_at = 0;

// page indices match the display lambda: 1 numbers, 2-4 graphs
inline bool page_needs_history(int p) { return !device_only && p >= 2 && p <= 4; }
inline bool page_needs_live(int p) { return !device_only && p == 1; }

// Whether this wake should wait for the network at all: only when the shown
// page consumes HA data or the clock has not been synced for a day.
inline bool network_needed() {
  time_t now = ::time(nullptr);
  bool time_stale = now < MIN_VALID_EPOCH || time_sync_at == 0 || now - time_sync_at > 24 * 3600;
  return time_stale || page_needs_live(page) || page_needs_history(page);
}

// A button flip onto a data page with nothing to show yet justifies an
// immediate network refresh.
inline bool page_data_empty(int p) {
  if (p == 1)
    return page_needs_live(p) && last_val_time == 0;
  if (p == 2 || p == 3)
    return page_needs_history(p) && fetch_time == 0;
  if (p == 4)
    return !device_only && week_fetch_time == 0;
  return false;
}

// --- History fetch ----------------------------------------------------------

// days since 1970-01-01 (Howard Hinnant's days-from-civil)
inline long days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned) (y - era * 400);
  unsigned doy = (153u * (unsigned) (m + (m > 2 ? -3 : 9)) + 2u) / 5u + (unsigned) d - 1u;
  unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return (long) era * 146097L + (long) doe - 719468L;
}

inline time_t parse_iso_utc(const char *s) {
  int y, mo, d, h, mi, sec;
  if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6)
    return 0;
  // HA reports last_changed in UTC
  return (time_t) days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec;
}

// Consumes the HA history JSON as a stream, splitting on '}' so no full-body
// buffer is needed. minimal_response entries carry state and last_changed in
// one chunk; the full first/last records get split by their attributes object,
// so a state seen without a timestamp is held pending until its timestamp
// arrives in the next chunk.
class HistoryScanner : public Stream {
 public:
  HistoryScanner(time_t start, time_t bucket, double *sum, int *cnt)
      : start_(start), bucket_(bucket), sum_(sum), cnt_(cnt) {}

  size_t write(uint8_t c) override {
    if (c == '}') {
      buf_[len_] = 0;
      this->entry_();
      len_ = 0;
    } else if (len_ < sizeof(buf_) - 1) {
      buf_[len_++] = (char) c;
    }
    return 1;
  }
  size_t write(const uint8_t *data, size_t n) override {
    for (size_t i = 0; i < n; i++)
      this->write(data[i]);
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }

  float baseline = NAN;  // last state before the window start, seeds forward-fill

 private:
  static const char *value_(const char *key_end) {
    while (*key_end == ':' || *key_end == ' ' || *key_end == '"')
      key_end++;
    return key_end;
  }

  void entry_() {
    const char *s = strstr(buf_, "\"state\"");
    const char *t = strstr(buf_, "\"last_changed\"");
    bool has_val = false;
    float val = NAN;
    if (s != nullptr) {
      const char *v = value_(s + 7);
      char *end;
      val = strtof(v, &end);
      has_val = end != v;  // non-numeric state: unavailable/unknown
    }
    if (s != nullptr && t == nullptr) {  // first half of a split full record
      pending_ = val;
      pending_valid_ = has_val;
      return;
    }
    if (t == nullptr)
      return;
    if (s == nullptr) {  // timestamp half of a split full record
      if (!pending_valid_)
        return;
      val = pending_;
      has_val = true;
      pending_valid_ = false;
    }
    if (!has_val)
      return;
    time_t ts = parse_iso_utc(value_(t + 14));
    if (ts == 0)
      return;
    if (ts < start_) {
      baseline = val;
      return;
    }
    long idx = (long) ((ts - start_) / bucket_);
    if (idx >= POINTS)
      idx = POINTS - 1;
    sum_[idx] += val;
    cnt_[idx]++;
  }

  time_t start_;
  time_t bucket_;
  double *sum_;
  int *cnt_;
  char buf_[512];
  size_t len_ = 0;
  float pending_ = NAN;
  bool pending_valid_ = false;
};

inline bool fetch_entity(const char *base, const char *token, const char *entity, time_t start,
                         time_t bucket, float *out) {
  static double sum[POINTS];
  static int cnt[POINTS];
  memset(sum, 0, sizeof(sum));
  memset(cnt, 0, sizeof(cnt));

  // end_time is mandatory: without it HA defaults the window to one day
  char iso[24], iso_end[24];
  struct tm tm_utc;
  gmtime_r(&start, &tm_utc);
  strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
  time_t end = start + bucket * POINTS;
  gmtime_r(&end, &tm_utc);
  strftime(iso_end, sizeof(iso_end), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
  char url[300];
  snprintf(url, sizeof(url),
           "%s/api/history/period/%s?end_time=%s&filter_entity_id=%s&minimal_response&no_attributes",
           base, iso, iso_end, entity);

  HTTPClient http;
  http.useHTTP10(true);
  http.setConnectTimeout(5000);
  http.setTimeout(15000);
  if (!http.begin(url)) {
    ESP_LOGW(TAG, "%s: http begin failed", entity);
    return false;
  }
  char auth[300];
  snprintf(auth, sizeof(auth), "Bearer %s", token);
  http.addHeader("Authorization", auth);
  int code = http.GET();
  if (code != 200) {
    ESP_LOGW(TAG, "%s: HTTP %d", entity, code);
    http.end();
    return false;
  }
  HistoryScanner scanner(start, bucket, sum, cnt);
  http.writeToStream(&scanner);
  http.end();

  float prev = scanner.baseline;
  int samples = 0;
  for (int i = 0; i < POINTS; i++) {
    if (cnt[i] > 0) {
      prev = (float) (sum[i] / cnt[i]);
      samples += cnt[i];
    }
    out[i] = prev;  // forward-fill gaps; NAN until the first sample
  }
  ESP_LOGI(TAG, "%s: %d samples", entity, samples);
  return samples > 0 || !std::isnan(scanner.baseline);
}

inline bool fetch_history(const char *base, const char *token) {
  time_t end = ::time(nullptr);
  if (end < MIN_VALID_EPOCH) {
    ESP_LOGW(TAG, "clock not set, skipping history fetch");
    return false;
  }
  // Stage so a partial failure keeps the previous, time-consistent data;
  // day and week windows commit independently.
  static float staging[SERIES_COUNT][POINTS];
  bool day_ok = true;
  for (int s = 0; s < SERIES_COUNT && day_ok; s++)
    day_ok = fetch_entity(base, token, ENTITIES[s], end - SPAN, SPAN / POINTS, staging[s]);
  if (day_ok) {
    memcpy(series, staging, sizeof(series));
    fetch_time = end;
  }
  bool week_ok = true;
  for (int s = 0; s < SERIES_COUNT && week_ok; s++)
    week_ok =
        fetch_entity(base, token, ENTITIES[s], end - SPAN_WEEK, SPAN_WEEK / POINTS, staging[s]);
  if (week_ok) {
    memcpy(series_week, staging, sizeof(series_week));
    week_fetch_time = end;
  }
  if (!day_ok || !week_ok)
    ESP_LOGW(TAG, "history fetch incomplete (day %d, week %d)", day_ok, week_ok);
  else
    ESP_LOGI(TAG, "history updated");
  return day_ok && week_ok;
}

// --- Device-only sampling ---------------------------------------------------

inline void clear_history() {
  fetch_time = 0;
  week_fetch_time = 0;
}

// Slide a window's buckets left so its end lands on `now`.
inline void roll_window(float rows[SERIES_COUNT][POINTS], time_t &end_t, time_t bucket,
                        time_t now) {
  if (end_t == 0 || now < end_t || now - end_t > bucket * POINTS) {
    for (int s = 0; s < SERIES_COUNT; s++)
      for (int i = 0; i < POINTS; i++)
        rows[s][i] = NAN;
    end_t = now;
    return;
  }
  int shift = (int) ((now - end_t) / bucket);
  if (shift > 0) {
    for (int s = 0; s < SERIES_COUNT; s++) {
      memmove(&rows[s][0], &rows[s][shift], (POINTS - shift) * sizeof(float));
      for (int i = POINTS - shift; i < POINTS; i++)
        rows[s][i] = NAN;
    }
    end_t += (time_t) shift * bucket;
  }
}

// Device-only mode: build the graph history from the onboard sensor, one
// sample per wake, into the same buffers the HA fetch would fill.
inline void sample_device(float t, float h) {
  time_t now = ::time(nullptr);
  if (now < MIN_VALID_EPOCH || std::isnan(t) || std::isnan(h))
    return;
  roll_window(series, fetch_time, SPAN / POINTS, now);
  roll_window(series_week, week_fetch_time, SPAN_WEEK / POINTS, now);
  series[S_IN_T][POINTS - 1] = t;
  series[S_IN_H][POINTS - 1] = h;
  series_week[S_IN_T][POINTS - 1] = t;
  series_week[S_IN_H][POINTS - 1] = h;
}

// --- Pages ------------------------------------------------------------------

// Newlib's POSIX TZ handling on the ESP32 is unreliable (per-row setenv/tzset
// rendered every zone in the system timezone), so offsets are computed
// explicitly. All DST rules here are "week'th Sunday of month at local hour";
// Tehran and Mexico City no longer observe DST.
struct Zone {
  const char *city;
  int std_min;  // standard offset from UTC, minutes
  int dst_min;  // == std_min when the zone has no DST
  int start_month, start_week, start_hour;  // DST begins
  int end_month, end_week, end_hour;        // DST ends
};

static const Zone ZONES[5] = {
    {"Tehran", 210, 210, 0, 0, 0, 0, 0, 0},
    {"Berlin", 60, 120, 3, 5, 2, 10, 5, 3},
    {"Memphis", -360, -300, 3, 2, 2, 11, 1, 2},
    {"Mexico City", -360, -360, 0, 0, 0, 0, 0, 0},
    {"Vancouver", -480, -420, 3, 2, 2, 11, 1, 2},
};

// Index into ZONES: highlighted on both clock pages and the base for the
// displayed offsets. Set from the Home Timezone select on every boot.
static int home_zone = 1;

// UTC epoch of the week'th Sunday (week 5 = last) of month at a local hour;
// offset_min is the zone offset in effect before the transition.
inline time_t transition_epoch(int year, int month, int week, int hour, int offset_min) {
  static const int MDAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  long first = days_from_civil(year, month, 1);
  int day;
  if (week == 5) {
    int dim = MDAYS[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
      dim++;
    int last_dow = (int) ((first + dim - 1 + 4) % 7);  // 0 = Sunday
    day = dim - last_dow;
  } else {
    int first_dow = (int) ((first + 4) % 7);
    day = 1 + (7 - first_dow) % 7 + (week - 1) * 7;
  }
  return (time_t) days_from_civil(year, month, day) * 86400 + hour * 3600 - offset_min * 60;
}

inline int zone_offset_min(const Zone &z, time_t t) {
  if (z.dst_min == z.std_min)
    return z.std_min;
  struct tm g;
  gmtime_r(&t, &g);
  int year = g.tm_year + 1900;
  time_t dst_start = transition_epoch(year, z.start_month, z.start_week, z.start_hour, z.std_min);
  time_t dst_end = transition_epoch(year, z.end_month, z.end_week, z.end_hour, z.dst_min);
  return (t >= dst_start && t < dst_end) ? z.dst_min : z.std_min;
}

// Offset of the system timezone (installed by HA) in minutes, derived by
// comparing the local and UTC calendars; used to highlight the local zone.
inline int local_offset_min(time_t t) {
  struct tm lt, gt;
  localtime_r(&t, &lt);
  gmtime_r(&t, &gt);
  long days = days_from_civil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday) -
              days_from_civil(gt.tm_year + 1900, gt.tm_mon + 1, gt.tm_mday);
  return (int) (days * 1440 + (lt.tm_hour - gt.tm_hour) * 60 + (lt.tm_min - gt.tm_min));
}

inline void draw_world_clock(Display &it, BaseFont *big, BaseFont *med, BaseFont *small_f) {
  time_t now = ::time(nullptr);
  it.print(400, 12, med, TextAlign::TOP_CENTER, "World Clock");
  it.line(20, 55, 780, 55);
  if (now < MIN_VALID_EPOCH) {
    it.print(400, 220, med, TextAlign::CENTER, "Waiting for time sync...");
    return;
  }

  int base_off = zone_offset_min(ZONES[home_zone], now);
  int y = 75;
  for (int zi = 0; zi < 5; zi++) {
    const Zone &z = ZONES[zi];
    int off = zone_offset_min(z, now);
    time_t local = now + (time_t) off * 60;
    struct tm lt;
    gmtime_r(&local, &lt);
    char hhmm[8], date[16];
    strftime(hhmm, sizeof(hhmm), "%H:%M", &lt);
    strftime(date, sizeof(date), "%a %d %b", &lt);
    bool is_local = zi == home_zone;
    auto fg = is_local ? esphome::display::COLOR_OFF : esphome::display::COLOR_ON;
    if (is_local)
      it.filled_rectangle(0, y - 10, 800, 66);
    it.print(40, y, big, fg, TextAlign::TOP_LEFT, z.city);
    if (!is_local) {
      int rel = off - base_off;
      int a = rel < 0 ? -rel : rel;
      it.printf(430, y + 24, small_f, fg, TextAlign::TOP_RIGHT, "%c%d:%02d",
                rel < 0 ? '-' : '+', a / 60, a % 60);
    }
    it.print(600, y + 24, small_f, fg, TextAlign::TOP_RIGHT, date);
    it.print(760, y, big, fg, TextAlign::TOP_RIGHT, hhmm);
    y += 80;
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

// Bottom status bar, inverted, drawn on every page. Left: Solar Hijri date;
// center: Gregorian date; right, growing leftwards: battery, SD-card icon,
// RTC warning, device climate.
inline void draw_status_bar(Display &it, BaseFont *small_f, float battery_pct, float dev_t,
                            float dev_h, bool show_climate, bool sd_present) {
  it.filled_rectangle(0, 446, 800, 34);
  time_t now = ::time(nullptr);
  if (now > MIN_VALID_EPOCH) {
    struct tm lt;
    localtime_r(&now, &lt);
    char greg[24];
    strftime(greg, sizeof(greg), "%a %d %b %Y", &lt);
    int jy, jm, jd;
    jalali::from_gregorian(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, jy, jm, jd);
    it.printf(20, 463, small_f, esphome::display::COLOR_OFF, TextAlign::CENTER_LEFT,
              "%d %s %d", jd, jalali::MONTHS[jm - 1], jy);
    it.print(400, 463, small_f, esphome::display::COLOR_OFF, TextAlign::CENTER, greg);
  }
  int x = 780;
  if (!std::isnan(battery_pct)) {
    it.printf(x, 463, small_f, esphome::display::COLOR_OFF, TextAlign::CENTER_RIGHT, "%.0f%%",
              battery_pct);
    x -= 58;
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
                         BaseFont *small_f, float dev_t, float dev_h) {
  if (device_only) {
    it.print(400, 40, med, TextAlign::TOP_CENTER, "Device");
    if (std::isnan(dev_t))
      it.print(400, 140, large, TextAlign::TOP_CENTER, "--");
    else
      it.printf(400, 130, huge, TextAlign::TOP_CENTER, "%.1f°C", dev_t);
    if (std::isnan(dev_h))
      it.print(400, 290, large, TextAlign::TOP_CENTER, "--");
    else
      it.printf(400, 280, huge, TextAlign::TOP_CENTER, "%.0f%%", dev_h);
    return;
  }
  const struct {
    int x;
    const char *title;
    float t;
    float h;
  } cols[3] = {{133, "Device", dev_t, dev_h},
               {400, "Indoor", last_val[S_IN_T], last_val[S_IN_H]},
               {667, "Outdoor", last_val[S_OUT_T], last_val[S_OUT_H]}};
  it.line(267, 50, 267, 400);
  it.line(533, 50, 533, 400);
  for (const auto &c : cols) {
    it.print(c.x, 40, med, TextAlign::TOP_CENTER, c.title);
    if (std::isnan(c.t))
      it.print(c.x, 140, large, TextAlign::TOP_CENTER, "--");
    else
      it.printf(c.x, 130, huge, TextAlign::TOP_CENTER, "%.1f°C", c.t);
    if (std::isnan(c.h))
      it.print(c.x, 290, large, TextAlign::TOP_CENTER, "--");
    else
      it.printf(c.x, 280, huge, TextAlign::TOP_CENTER, "%.0f%%", c.h);
  }
}

// Draws into the [top, bottom] band so it serves full and half pages alike.
inline void draw_graph_page(Display &it, Series sa, Series sb, const char *title,
                            const char *unit, BaseFont *med, BaseFont *small_f, int top,
                            int bottom) {
  it.print(20, top + 4, med, TextAlign::TOP_LEFT, title);
  int mid = (top + bottom) / 2;
  if (fetch_time == 0) {
    it.print(400, mid, med, TextAlign::CENTER, "No history yet");
    it.print(400, mid + 40, small_f, TextAlign::CENTER, "press the green button to fetch");
    return;
  }

  float lo = NAN, hi = NAN;
  for (int s = 0; s < 2; s++) {
    const float *ser = series[s == 0 ? sa : sb];
    for (int i = 0; i < POINTS; i++) {
      float v = ser[i];
      if (std::isnan(v))
        continue;
      if (std::isnan(lo) || v < lo)
        lo = v;
      if (std::isnan(hi) || v > hi)
        hi = v;
    }
  }
  if (std::isnan(lo)) {
    it.print(400, mid, med, TextAlign::CENTER, "No data");
    return;
  }
  if (hi - lo < 1.0f) {
    lo -= 0.5f;
    hi += 0.5f;
  }
  const int X0 = 70, X1 = 780, Y0 = top + 48, Y1 = bottom - 28;
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
    it.printf(X0 - 8, y, small_f, TextAlign::CENTER_RIGHT, "%g", (double) v);
  }

  time_t start = fetch_time - SPAN;
  struct tm lt;
  localtime_r(&start, &lt);
  time_t first = start + (4 * 3600 - ((lt.tm_hour % 4) * 3600 + lt.tm_min * 60 + lt.tm_sec));
  for (time_t t = first; t <= fetch_time; t += 4 * 3600) {
    int x = X0 + (int) ((t - start) * (long) (X1 - X0) / SPAN);
    for (int y = Y0; y <= Y1; y += 4)
      it.draw_pixel_at(x, y);
    localtime_r(&t, &lt);
    it.printf(x, Y1 + 6, small_f, TextAlign::TOP_CENTER, "%02d:00", lt.tm_hour);
  }
  it.rectangle(X0, Y0, X1 - X0 + 1, Y1 - Y0 + 1);

  // inside solid (doubled for thickness), outside dashed
  for (int pass = 0; pass < 2; pass++) {
    const float *ser = series[pass == 0 ? sa : sb];
    bool dashed = pass == 1;
    for (int i = 0; i + 1 < POINTS; i++) {
      if (std::isnan(ser[i]) || std::isnan(ser[i + 1]))
        continue;
      if (dashed && (i % 4) >= 2)
        continue;
      it.line(sx(i), sy(ser[i]), sx(i + 1), sy(ser[i + 1]));
      if (!dashed)
        it.line(sx(i), sy(ser[i]) + 1, sx(i + 1), sy(ser[i + 1]) + 1);
    }
  }

  // legend on the title line, right end
  it.line(460, top + 20, 496, top + 20);
  it.line(460, top + 21, 496, top + 21);
  if (std::isnan(last_val[sa]))
    it.print(504, top + 10, small_f, TextAlign::TOP_LEFT, "in");
  else
    it.printf(504, top + 10, small_f, TextAlign::TOP_LEFT, "in %.1f%s", last_val[sa], unit);
  for (int x = 620; x < 656; x += 8) {
    it.line(x, top + 20, x + 4, top + 20);
    it.line(x, top + 21, x + 4, top + 21);
  }
  if (std::isnan(last_val[sb]))
    it.print(664, top + 10, small_f, TextAlign::TOP_LEFT, "out");
  else
    it.printf(664, top + 10, small_f, TextAlign::TOP_LEFT, "out %.1f%s", last_val[sb], unit);
}

// Combined 7-day page: temperature and humidity stacked, one legend and data
// stamp, shared day axis at the bottom, vertical inverted section titles.
inline void draw_week_page(Display &it, BaseFont *med, BaseFont *small_f) {
  if (week_fetch_time == 0) {
    it.print(400, 220, med, TextAlign::CENTER, "No history yet");
    it.print(400, 260, small_f, TextAlign::CENTER, "press the green button to fetch");
    return;
  }
  const int X0 = 84, X1 = 784;

  // one legend for both bands, centered so it reads as global
  // (device-only mode has a single unlabeled series)
  if (!device_only) {
    it.line(300, 14, 336, 14);
    it.line(300, 15, 336, 15);
    it.print(344, 4, small_f, TextAlign::TOP_LEFT, "in");
    for (int x = 410; x < 446; x += 8) {
      it.line(x, 14, x + 4, 14);
      it.line(x, 15, x + 4, 15);
    }
    it.print(454, 4, small_f, TextAlign::TOP_LEFT, "out");
  }

  // one local-shifted time base shared by both bands and the day labels
  int loff = local_offset_min(week_fetch_time);
  time_t lstart = week_fetch_time - SPAN_WEEK + (time_t) loff * 60;
  time_t midnight0 = lstart - (lstart % 86400) + 86400;

  const struct {
    Series a, b;
    const char *tag;
    int y0, y1;
  // 44 px gap between bands so their axis numbers cannot collide
  } BANDS[2] = {{S_IN_T, S_OUT_T, "TEMP", 34, 196}, {S_IN_H, S_OUT_H, "HUM", 240, 408}};

  for (const auto &band : BANDS) {
    it.filled_rectangle(0, band.y0, 32, band.y1 - band.y0);
    int n = (int) strlen(band.tag);
    int ty = (band.y0 + band.y1) / 2 - n * 15;
    for (int i = 0; i < n; i++) {
      char c[2] = {band.tag[i], 0};
      it.print(16, ty + i * 30, small_f, esphome::display::COLOR_OFF, TextAlign::TOP_CENTER, c);
    }

    int nseries = device_only ? 1 : 2;
    float lo = NAN, hi = NAN;
    for (int s = 0; s < nseries; s++) {
      const float *ser = series_week[s == 0 ? band.a : band.b];
      for (int i = 0; i < POINTS; i++) {
        float v = ser[i];
        if (std::isnan(v))
          continue;
        if (std::isnan(lo) || v < lo)
          lo = v;
        if (std::isnan(hi) || v > hi)
          hi = v;
      }
    }
    if (std::isnan(lo))
      continue;
    if (hi - lo < 1.0f) {
      lo -= 0.5f;
      hi += 0.5f;
    }
    static const float STEPS[] = {0.5f, 1, 2, 5, 10, 20, 25, 50};
    float step = STEPS[0];
    for (float s : STEPS) {
      step = s;
      if ((hi - lo) / s <= 4.0f)
        break;
    }
    float glo = floorf(lo / step) * step;
    float ghi = ceilf(hi / step) * step;
    const int Y0 = band.y0, Y1 = band.y1;
    auto sx = [&](int i) { return X0 + i * (X1 - X0) / (POINTS - 1); };
    auto sy = [&](float v) { return Y1 - (int) ((v - glo) * (float) (Y1 - Y0) / (ghi - glo)); };

    for (float v = glo; v <= ghi + step / 2; v += step) {
      int y = sy(v);
      for (int x = X0; x <= X1; x += 4)
        it.draw_pixel_at(x, y);
      it.printf(X0 - 8, y, small_f, TextAlign::CENTER_RIGHT, "%g", (double) v);
    }
    for (time_t lm = midnight0; lm < lstart + SPAN_WEEK; lm += 86400) {
      int x = X0 + (int) ((lm - lstart) * (long) (X1 - X0) / SPAN_WEEK);
      for (int y = Y0; y <= Y1; y += 4)
        it.draw_pixel_at(x, y);
    }
    it.rectangle(X0, Y0, X1 - X0 + 1, Y1 - Y0 + 1);

    for (int pass = 0; pass < nseries; pass++) {
      const float *ser = series_week[pass == 0 ? band.a : band.b];
      bool dashed = pass == 1;
      for (int i = 0; i + 1 < POINTS; i++) {
        if (std::isnan(ser[i]) || std::isnan(ser[i + 1]))
          continue;
        if (dashed && (i % 4) >= 2)
          continue;
        it.line(sx(i), sy(ser[i]), sx(i + 1), sy(ser[i + 1]));
        if (!dashed)
          it.line(sx(i), sy(ser[i]) + 1, sx(i + 1), sy(ser[i + 1]) + 1);
      }
    }
  }

  // weekday labels centered per day, once, under the bottom band
  for (time_t lm = midnight0 - 86400; lm < lstart + SPAN_WEEK; lm += 86400) {
    time_t center = lm + 43200;
    if (center < lstart || center > lstart + SPAN_WEEK)
      continue;
    int x = X0 + (int) ((center - lstart) * (long) (X1 - X0) / SPAN_WEEK);
    struct tm dt;
    gmtime_r(&center, &dt);
    char day[8];
    strftime(day, sizeof(day), "%a", &dt);
    it.print(x, 414, small_f, TextAlign::TOP_CENTER, day);
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

// Five analog world clocks, 3 + 2 layout.
inline void draw_analog_clocks(Display &it, BaseFont *med, BaseFont *small_f) {
  time_t now = ::time(nullptr);
  if (now < MIN_VALID_EPOCH) {
    it.print(400, 220, med, TextAlign::CENTER, "Waiting for time sync...");
    return;
  }
  constexpr float PI_F = 3.14159265f;
  const struct {
    int cx, cy;
  } POS[5] = {{133, 124}, {400, 124}, {667, 124}, {266, 318}, {533, 318}};
  const int R = 96;
  int base_off = zone_offset_min(ZONES[home_zone], now);
  for (int i = 0; i < 5; i++) {
    const Zone &z = ZONES[i];
    int off = zone_offset_min(z, now);
    time_t local = now + (time_t) off * 60;
    struct tm lt;
    gmtime_r(&local, &lt);
    int cx = POS[i].cx, cy = POS[i].cy;
    bool is_local = i == home_zone;
    // inverted dial = that zone is in its night hours (18:00-06:00)
    bool night = lt.tm_hour < 6 || lt.tm_hour >= 18;
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
                 quarter ? 5 : 3, fg);
    }
    float ah = ((lt.tm_hour % 12) + lt.tm_min / 60.0f) * PI_F / 6.0f;
    float am = lt.tm_min * PI_F / 30.0f;
    thick_line(it, cx, cy, cx + (int) (sinf(ah) * R * 0.52f), cy - (int) (cosf(ah) * R * 0.52f),
               7, fg);
    thick_line(it, cx, cy, cx + (int) (sinf(am) * R * 0.85f), cy - (int) (cosf(am) * R * 0.85f),
               5, fg);
    it.filled_circle(cx, cy, 6, fg);
    // the local zone is marked by an inverted caption instead of its dial
    char label[32];
    if (is_local) {
      snprintf(label, sizeof(label), "%s", z.city);
    } else {
      int rel = off - base_off;
      int a2 = rel < 0 ? -rel : rel;
      snprintf(label, sizeof(label), "%s %c%d:%02d", z.city, rel < 0 ? '-' : '+', a2 / 60,
               a2 % 60);
    }
    int ly = cy + R + 10;
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

}  // namespace reterminal
