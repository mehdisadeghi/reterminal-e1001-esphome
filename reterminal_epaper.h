#pragma once

#include <HTTPClient.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esphome/components/display/display.h"
#include "esphome/core/log.h"

namespace reterminal {

using esphome::display::BaseFont;
using esphome::display::Display;
using esphome::display::TextAlign;

static const char *const TAG = "reterminal";

constexpr int PAGE_COUNT = 4;  // 0 world clock, 1 big numbers, 2 temp graph, 3 hum graph
constexpr int POINTS = 192;    // 24 h in 7.5 min buckets
constexpr time_t SPAN = 24 * 3600;
constexpr time_t BUCKET = SPAN / POINTS;
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
RTC_DATA_ATTR float last_val[SERIES_COUNT] = {NAN, NAN, NAN, NAN};
RTC_DATA_ATTR time_t last_val_time = 0;

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

inline void prev_page() { page = (page + PAGE_COUNT - 1) % PAGE_COUNT; }
inline void next_page() { page = (page + 1) % PAGE_COUNT; }

// --- History fetch ----------------------------------------------------------

inline time_t parse_iso_utc(const char *s) {
  int y, mo, d, h, mi, sec;
  if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6)
    return 0;
  // days-from-civil (Howard Hinnant); HA reports last_changed in UTC
  y -= mo <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned) (y - era * 400);
  unsigned doy = (153u * (unsigned) (mo + (mo > 2 ? -3 : 9)) + 2u) / 5u + (unsigned) d - 1u;
  unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  long days = (long) era * 146097L + (long) doe - 719468L;
  return (time_t) days * 86400 + h * 3600 + mi * 60 + sec;
}

// Consumes the HA history JSON as a stream, splitting on '}' so no full-body
// buffer is needed. minimal_response entries carry state and last_changed in
// one chunk; the full first/last records get split by their attributes object,
// so a state seen without a timestamp is held pending until its timestamp
// arrives in the next chunk.
class HistoryScanner : public Stream {
 public:
  HistoryScanner(time_t start, double *sum, int *cnt) : start_(start), sum_(sum), cnt_(cnt) {}

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
    long idx = (long) ((ts - start_) / BUCKET);
    if (idx >= POINTS)
      idx = POINTS - 1;
    sum_[idx] += val;
    cnt_[idx]++;
  }

  time_t start_;
  double *sum_;
  int *cnt_;
  char buf_[512];
  size_t len_ = 0;
  float pending_ = NAN;
  bool pending_valid_ = false;
};

inline bool fetch_entity(const char *base, const char *token, const char *entity, time_t start,
                         float *out) {
  static double sum[POINTS];
  static int cnt[POINTS];
  memset(sum, 0, sizeof(sum));
  memset(cnt, 0, sizeof(cnt));

  char iso[24];
  struct tm tm_utc;
  gmtime_r(&start, &tm_utc);
  strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
  char url[256];
  snprintf(url, sizeof(url),
           "%s/api/history/period/%s?filter_entity_id=%s&minimal_response&no_attributes", base,
           iso, entity);

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
  HistoryScanner scanner(start, sum, cnt);
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
  time_t start = end - SPAN;
  // Stage so a partial failure keeps the previous, time-consistent data.
  static float staging[SERIES_COUNT][POINTS];
  for (int s = 0; s < SERIES_COUNT; s++) {
    if (!fetch_entity(base, token, ENTITIES[s], start, staging[s])) {
      ESP_LOGW(TAG, "history fetch failed, keeping previous data");
      return false;
    }
  }
  memcpy(series, staging, sizeof(series));
  fetch_time = end;
  ESP_LOGI(TAG, "history updated");
  return true;
}

// --- Pages ------------------------------------------------------------------

struct Zone {
  const char *city;
  const char *tz;
};

// POSIX TZ strings; Tehran and Mexico City no longer observe DST.
static const Zone ZONES[5] = {
    {"Tehran", "<+0330>-3:30"},
    {"Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Memphis", "CST6CDT,M3.2.0,M11.1.0"},
    {"Mexico City", "CST6"},
    {"Vancouver", "PST8PDT,M3.2.0,M11.1.0"},
};

inline void draw_world_clock(Display &it, BaseFont *big, BaseFont *med, BaseFont *small_f) {
  time_t now = ::time(nullptr);
  it.print(400, 12, med, TextAlign::TOP_CENTER, "World Clock");
  it.line(20, 55, 780, 55);
  if (now < MIN_VALID_EPOCH) {
    it.print(400, 220, med, TextAlign::CENTER, "Waiting for time sync...");
    return;
  }

  char saved[64] = "";
  const char *tz = getenv("TZ");
  if (tz != nullptr)
    snprintf(saved, sizeof(saved), "%s", tz);

  int y = 75;
  for (const auto &z : ZONES) {
    setenv("TZ", z.tz, 1);
    tzset();
    struct tm lt;
    localtime_r(&now, &lt);
    char hhmm[8], date[16];
    strftime(hhmm, sizeof(hhmm), "%H:%M", &lt);
    strftime(date, sizeof(date), "%a %d %b", &lt);
    it.print(40, y, big, TextAlign::TOP_LEFT, z.city);
    it.print(600, y + 24, small_f, TextAlign::TOP_RIGHT, date);
    it.print(760, y, big, TextAlign::TOP_RIGHT, hhmm);
    y += 80;
  }

  if (saved[0] != 0)
    setenv("TZ", saved, 1);
  else
    unsetenv("TZ");
  tzset();
}

inline void draw_numbers(Display &it, BaseFont *huge, BaseFont *large, BaseFont *med,
                         BaseFont *small_f, float battery_pct) {
  it.print(200, 40, med, TextAlign::TOP_CENTER, "Indoor");
  it.print(600, 40, med, TextAlign::TOP_CENTER, "Outdoor");
  it.line(400, 50, 400, 400);

  const struct {
    int x;
    Series t;
    Series h;
  } cols[2] = {{200, S_IN_T, S_IN_H}, {600, S_OUT_T, S_OUT_H}};
  for (const auto &c : cols) {
    if (std::isnan(last_val[c.t]))
      it.print(c.x, 140, large, TextAlign::TOP_CENTER, "--");
    else
      it.printf(c.x, 130, huge, TextAlign::TOP_CENTER, "%.1f°C", last_val[c.t]);
    if (std::isnan(last_val[c.h]))
      it.print(c.x, 290, large, TextAlign::TOP_CENTER, "--");
    else
      it.printf(c.x, 280, huge, TextAlign::TOP_CENTER, "%.0f%%", last_val[c.h]);
  }

  // status bar, inverted: refresh datetime left, battery right
  it.filled_rectangle(0, 446, 800, 34);
  time_t now = ::time(nullptr);
  if (now > MIN_VALID_EPOCH) {
    struct tm lt;
    localtime_r(&now, &lt);
    char stamp[24];
    strftime(stamp, sizeof(stamp), "%d %b %H:%M", &lt);
    it.print(20, 463, small_f, esphome::display::COLOR_OFF, TextAlign::CENTER_LEFT, stamp);
  }
  if (!std::isnan(battery_pct))
    it.printf(780, 463, small_f, esphome::display::COLOR_OFF, TextAlign::CENTER_RIGHT, "%.0f%%",
              battery_pct);
}

inline void draw_graph_page(Display &it, Series sa, Series sb, const char *title,
                            const char *unit, BaseFont *med, BaseFont *small_f) {
  it.print(20, 8, med, TextAlign::TOP_LEFT, title);
  if (fetch_time == 0) {
    it.print(400, 220, med, TextAlign::CENTER, "No history yet");
    it.print(400, 260, small_f, TextAlign::CENTER, "press the green button to fetch");
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
    it.print(400, 220, med, TextAlign::CENTER, "No data");
    return;
  }
  if (hi - lo < 1.0f) {
    lo -= 0.5f;
    hi += 0.5f;
  }
  static const float STEPS[] = {0.5f, 1, 2, 5, 10, 20, 25, 50};
  float step = STEPS[0];
  for (float s : STEPS) {
    step = s;
    if ((hi - lo) / s <= 6.0f)
      break;
  }
  float glo = floorf(lo / step) * step;
  float ghi = ceilf(hi / step) * step;

  const int X0 = 70, X1 = 780, Y0 = 80, Y1 = 418;
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
    it.printf(x, Y1 + 8, small_f, TextAlign::TOP_CENTER, "%02d:00", lt.tm_hour);
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

  it.line(20, 56, 56, 56);
  it.line(20, 57, 56, 57);
  if (std::isnan(last_val[sa]))
    it.print(64, 46, small_f, TextAlign::TOP_LEFT, "in");
  else
    it.printf(64, 46, small_f, TextAlign::TOP_LEFT, "in %.1f%s", last_val[sa], unit);
  for (int x = 210; x < 246; x += 8) {
    it.line(x, 56, x + 4, 56);
    it.line(x, 57, x + 4, 57);
  }
  if (std::isnan(last_val[sb]))
    it.print(254, 46, small_f, TextAlign::TOP_LEFT, "out");
  else
    it.printf(254, 46, small_f, TextAlign::TOP_LEFT, "out %.1f%s", last_val[sb], unit);

  localtime_r(&fetch_time, &lt);
  it.printf(780, 14, small_f, TextAlign::TOP_RIGHT, "as of %02d:%02d", lt.tm_hour, lt.tm_min);
}

}  // namespace reterminal
