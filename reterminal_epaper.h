#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SD.h>
#include <SPI.h>
#include <strings.h>
#include <sys/time.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include "esp_attr.h"
#include "esp_partition.h"
#include "esp_sleep.h"
#include "esphome/components/display/display.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "jalali.h"
#include "tzdata.h"

namespace reterminal {

using esphome::display::BaseFont;
using esphome::display::Display;
using esphome::display::TextAlign;

static const char *const TAG = "reterminal";

constexpr int PAGE_COUNT = 7;  // world clock, numbers, temp, hum, combined, analog, noqte
constexpr int POINTS = 192;    // buckets per window
constexpr time_t SPAN = 24 * 3600;
constexpr time_t SPAN_WEEK = 7 * 24 * 3600;
constexpr time_t FETCH_INTERVAL = 3 * 3600;
constexpr time_t MIN_VALID_EPOCH = 1600000000;  // clock has been synced at least once

// --- Climate columns: sources are data, not code -----------------------------
// Column spec: "Label=dev" (onboard SHT4x) or
// "Label=<temp_entity>,<hum_entity>" (polled from HA over REST).

constexpr int MAX_COLS = 3;
constexpr int M_TEMP = 0, M_HUM = 1;

struct Column {
  bool enabled;
  bool is_dev;
  char label[16];
  char temp_entity[64];
  char hum_entity[64];
};

static Column cols[MAX_COLS];

inline bool parse_column(const char *s, Column &c) {
  const char *eq = strchr(s, '=');
  if (eq == nullptr || eq == s)
    return false;
  size_t n = (size_t) (eq - s);
  if (n >= sizeof(c.label))
    return false;
  memcpy(c.label, s, n);
  c.label[n] = 0;
  const char *v = eq + 1;
  if (strcmp(v, "dev") == 0) {
    c.is_dev = true;
    c.temp_entity[0] = c.hum_entity[0] = 0;
    return true;
  }
  const char *comma = strchr(v, ',');
  if (comma == nullptr)
    return false;
  size_t tn = (size_t) (comma - v), hn = strlen(comma + 1);
  if (tn == 0 || tn >= sizeof(c.temp_entity) || hn == 0 || hn >= sizeof(c.hum_entity))
    return false;
  memcpy(c.temp_entity, v, tn);
  c.temp_entity[tn] = 0;
  snprintf(c.hum_entity, sizeof(c.hum_entity), "%s", comma + 1);
  c.is_dev = false;
  return true;
}

inline void rebuild_columns(const char *c1, const char *c2, const char *c3) {
  const char *in[MAX_COLS] = {c1, c2, c3};
  for (int i = 0; i < MAX_COLS; i++) {
    cols[i].enabled = false;
    if (in[i] == nullptr || in[i][0] == 0)
      continue;
    Column c{};
    if (parse_column(in[i], c)) {
      c.enabled = true;
      cols[i] = c;
    } else {
      ESP_LOGW(TAG, "invalid column spec: %s", in[i]);
    }
  }
}

inline bool any_ha_col() {
  for (int i = 0; i < MAX_COLS; i++)
    if (cols[i].enabled && !cols[i].is_dev)
      return true;
  return false;
}

// --- History storage: int16 (value*100) so both windows fit RTC memory ------

constexpr int16_t PT_NAN = INT16_MIN;

inline float pt_get(int16_t v) { return v == PT_NAN ? NAN : v / 100.0f; }
inline int16_t pt_set(float v) {
  if (std::isnan(v) || v < -327.0f || v > 327.0f)
    return PT_NAN;
  return (int16_t) lroundf(v * 100.0f);
}

// State in RTC slow memory: survives deep sleep; restored from the flash
// snapshot after a power loss or reflash.
RTC_DATA_ATTR int page = 0;
RTC_DATA_ATTR time_t fetch_time = 0;       // day window end; 0 = never rolled
RTC_DATA_ATTR time_t week_fetch_time = 0;  // week window end
RTC_DATA_ATTR time_t ha_fetch_at = 0;      // last successful HA history fetch
RTC_DATA_ATTR int16_t series_day[MAX_COLS][2][POINTS];
RTC_DATA_ATTR int16_t series_week[MAX_COLS][2][POINTS];
RTC_DATA_ATTR float last_val[MAX_COLS][2] = {{NAN, NAN}, {NAN, NAN}, {NAN, NAN}};
RTC_DATA_ATTR time_t last_val_time = 0;
RTC_DATA_ATTR time_t ha_live_at = 0;  // last successful /api/states poll
RTC_DATA_ATTR time_t beep_ack = 0;  // last handled "single beep" request
RTC_DATA_ATTR time_t sync_attempt_at = 0;

// Awake bookkeeping (plain RAM, reset each wake). Any command bumps the
// deadline a short grace ahead; an interval enters sleep once it passes and
// no script is running.
constexpr uint32_t AWAKE_GRACE_MS = 2000;      // autonomous work: sleep right after
constexpr uint32_t INTERACT_AWAKE_MS = 30000;  // user interaction: stay reachable
static uint32_t sleep_deadline = 0;
static bool ota_in_progress = false;

inline void bump_awake_for(uint32_t ms) {
  uint32_t d = millis() + ms;
  if ((int32_t) (d - sleep_deadline) > 0)
    sleep_deadline = d;
}

inline void bump_awake() { bump_awake_for(AWAKE_GRACE_MS); }

// For handlers a user can trigger. Config-entity restores fire the same
// on_value/on_turn_* hooks during the first seconds of every boot; real
// commands can only arrive once the system is up, so gate on uptime.
inline void bump_interact() {
  bump_awake_for(millis() > 8000 ? INTERACT_AWAKE_MS : AWAKE_GRACE_MS);
}

inline bool sleep_due() { return !ota_in_progress && millis() > sleep_deadline; }

// Mirrors of HA entities and boot diagnostics, set from YAML.
static bool radio_on = true;      // wifi enabled this wake
static bool rtc_bad = false;      // RTC chip unusable at boot (dead cell / VL / frozen)
static time_t pre_rtc_time = 0;   // system time captured before the RTC read
static bool force_sync = false;  // green button: sync + fetch now
static int sync_interval_min = 15;
static bool page_enabled_[PAGE_COUNT] = {true, true, true, true, true, true, true};

inline bool page_hidden(int p) { return !page_enabled_[p]; }

// Bounded loops: with every page hidden they fall back to page 0.
inline void prev_page() {
  for (int i = 0; i < PAGE_COUNT; i++) {
    page = (page + PAGE_COUNT - 1) % PAGE_COUNT;
    if (!page_hidden(page))
      return;
  }
  page = 0;
}
inline void next_page() {
  for (int i = 0; i < PAGE_COUNT; i++) {
    page = (page + 1) % PAGE_COUNT;
    if (!page_hidden(page))
      return;
  }
  page = 0;
}

// --- Wake economics ----------------------------------------------------------

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

// HA sync (API connect: time + queued helpers) runs on boot, on green, or
// every sync_interval_min — not on every wake.
inline bool sync_due() {
  time_t now = ::time(nullptr);
  if (now < MIN_VALID_EPOCH)
    return true;
  return sync_attempt_at == 0 || now - sync_attempt_at > (time_t) sync_interval_min * 60;
}

inline bool fetch_due() {
  time_t now = ::time(nullptr);
  if (now < MIN_VALID_EPOCH || !any_ha_col())
    return false;
  return ha_fetch_at == 0 || now - ha_fetch_at > FETCH_INTERVAL;
}

// page indices match the display lambda: 1 numbers, 2-4 graphs
inline bool page_needs_live(int p) { return p == 1 && any_ha_col(); }
inline bool page_needs_history(int p) { return p >= 2 && p <= 4 && any_ha_col(); }

inline bool network_needed() {
  return sync_due() || page_needs_live(page) || (page_needs_history(page) && fetch_due());
}

// A button flip onto a data page with missing or stale HA values earns a
// refresh. Dev columns have their own per-wake sampling and never trigger it.
inline bool page_data_empty(int p) {
  if (p == 1) {
    time_t now = ::time(nullptr);
    return page_needs_live(p) && (ha_live_at == 0 || now - ha_live_at > 600);
  }
  if (p >= 2 && p <= 4)
    return page_needs_history(p) && ha_fetch_at == 0;
  return false;
}

// --- Calendar helpers ---------------------------------------------------------

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

// --- Window rolling & device sampling ----------------------------------------

inline void roll_rows(int16_t rows[MAX_COLS][2][POINTS], time_t &end_t, time_t bucket,
                      time_t now) {
  if (end_t == 0 || now < end_t || now - end_t > bucket * POINTS) {
    for (int c = 0; c < MAX_COLS; c++)
      for (int m = 0; m < 2; m++)
        for (int i = 0; i < POINTS; i++)
          rows[c][m][i] = PT_NAN;
    end_t = now;
    return;
  }
  int shift = (int) ((now - end_t) / bucket);
  if (shift == 0)
    return;
  for (int c = 0; c < MAX_COLS; c++)
    for (int m = 0; m < 2; m++) {
      memmove(&rows[c][m][0], &rows[c][m][shift], (POINTS - shift) * sizeof(int16_t));
      for (int i = POINTS - shift; i < POINTS; i++)
        rows[c][m][i] = PT_NAN;
    }
  end_t += (time_t) shift * bucket;
}

// Once per wake: slide both windows to `now` and sample dev columns into
// their last bucket.
inline void tick_data(float dev_t, float dev_h) {
  time_t now = ::time(nullptr);
  if (now < MIN_VALID_EPOCH)
    return;
  roll_rows(series_day, fetch_time, SPAN / POINTS, now);
  roll_rows(series_week, week_fetch_time, SPAN_WEEK / POINTS, now);
  bool any = false;
  for (int i = 0; i < MAX_COLS; i++) {
    if (!cols[i].enabled || !cols[i].is_dev)
      continue;
    if (std::isnan(dev_t) || std::isnan(dev_h))
      continue;
    series_day[i][M_TEMP][POINTS - 1] = pt_set(dev_t);
    series_day[i][M_HUM][POINTS - 1] = pt_set(dev_h);
    series_week[i][M_TEMP][POINTS - 1] = pt_set(dev_t);
    series_week[i][M_HUM][POINTS - 1] = pt_set(dev_h);
    last_val[i][M_TEMP] = dev_t;
    last_val[i][M_HUM] = dev_h;
    any = true;
  }
  if (any)
    last_val_time = now;
}

// --- HA REST access -----------------------------------------------------------

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
                         time_t bucket, int16_t *out) {
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
    out[i] = pt_set(prev);  // forward-fill gaps; NAN until the first sample
  }
  ESP_LOGI(TAG, "%s: %d samples", entity, samples);
  return samples > 0 || !std::isnan(scanner.baseline);
}

// Both windows for every HA-sourced column; rows commit individually so a
// single failure keeps that row's previous (already rolled) data.
inline bool fetch_history(const char *base, const char *token) {
  time_t end = ::time(nullptr);
  if (end < MIN_VALID_EPOCH) {
    ESP_LOGW(TAG, "clock not set, skipping history fetch");
    return false;
  }
  if (!any_ha_col())
    return false;
  roll_rows(series_day, fetch_time, SPAN / POINTS, end);
  roll_rows(series_week, week_fetch_time, SPAN_WEEK / POINTS, end);
  static int16_t row[POINTS];
  bool all = true;
  for (int c = 0; c < MAX_COLS; c++) {
    if (!cols[c].enabled || cols[c].is_dev)
      continue;
    const char *ent[2] = {cols[c].temp_entity, cols[c].hum_entity};
    for (int m = 0; m < 2; m++) {
      if (fetch_entity(base, token, ent[m], end - SPAN, SPAN / POINTS, row))
        memcpy(series_day[c][m], row, sizeof(row));
      else
        all = false;
      if (fetch_entity(base, token, ent[m], end - SPAN_WEEK, SPAN_WEEK / POINTS, row))
        memcpy(series_week[c][m], row, sizeof(row));
      else
        all = false;
    }
  }
  if (all) {
    ha_fetch_at = end;
    ESP_LOGI(TAG, "history updated");
  } else {
    ESP_LOGW(TAG, "history fetch incomplete");
  }
  return all;
}

// Captures the first bytes of a response body (enough for /api/states).
class CaptureStream : public Stream {
 public:
  size_t write(uint8_t c) override {
    if (len_ < sizeof(buf_) - 1)
      buf_[len_++] = (char) c;
    return 1;
  }
  size_t write(const uint8_t *d, size_t n) override {
    for (size_t i = 0; i < n; i++)
      this->write(d[i]);
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  const char *c_str() {
    buf_[len_] = 0;
    return buf_;
  }

 private:
  char buf_[512];
  size_t len_ = 0;
};

// Current values of HA-sourced columns via /api/states; transport identical
// to the (proven) history fetch.
inline void poll_live(const char *base, const char *token) {
  bool any = false;
  for (int c = 0; c < MAX_COLS; c++) {
    if (!cols[c].enabled || cols[c].is_dev)
      continue;
    const char *ent[2] = {cols[c].temp_entity, cols[c].hum_entity};
    for (int m = 0; m < 2; m++) {
      char url[192];
      snprintf(url, sizeof(url), "%s/api/states/%s", base, ent[m]);
      HTTPClient http;
      http.useHTTP10(true);
      http.setConnectTimeout(5000);
      http.setTimeout(8000);
      if (!http.begin(url)) {
        ESP_LOGW(TAG, "%s: http begin failed", ent[m]);
        continue;
      }
      char auth[300];
      snprintf(auth, sizeof(auth), "Bearer %s", token);
      http.addHeader("Authorization", auth);
      int code = http.GET();
      if (code == 200) {
        CaptureStream cap;
        http.writeToStream(&cap);
        const char *p = strstr(cap.c_str(), "\"state\"");
        if (p != nullptr) {
          p += 7;
          while (*p == ':' || *p == ' ' || *p == '"')
            p++;
          char *endp;
          float v = strtof(p, &endp);
          if (endp != p) {
            last_val[c][m] = v;
            any = true;
            ESP_LOGI(TAG, "%s: live %.2f", ent[m], v);
          } else {
            ESP_LOGW(TAG, "%s: non-numeric state", ent[m]);
          }
        } else {
          ESP_LOGW(TAG, "%s: no state in response", ent[m]);
        }
      } else {
        ESP_LOGW(TAG, "%s: HTTP %d", ent[m], code);
      }
      http.end();
    }
  }
  if (any) {
    last_val_time = ::time(nullptr);
    ha_live_at = last_val_time;
  }
}

// --- History snapshots on the flash data partition ---------------------------
// Survive power loss and reflash; the 24 MB "hist" partition as a slot ring
// makes wear negligible (one erase cycle per slot every ~4 months at hourly
// snapshots).

constexpr uint32_t SNAP_MAGIC = 0x48545231;
constexpr uint32_t SNAP_SLOT = 8192;
constexpr time_t SNAP_INTERVAL = 3600;

struct Snapshot {
  uint32_t magic;
  uint32_t seq;
  int64_t day_end, week_end, ha_fetch, lv_time;
  float lv[MAX_COLS][2];
  int16_t day[MAX_COLS][2][POINTS];
  int16_t week[MAX_COLS][2][POINTS];
  uint32_t crc;
};

static const esp_partition_t *hist_part = nullptr;
RTC_DATA_ATTR uint32_t snap_seq = 0;
RTC_DATA_ATTR time_t snap_at = 0;

inline Snapshot &snap_buf() {
  static Snapshot s;
  return s;
}

inline uint32_t crc32_buf(const void *data, size_t len) {
  const uint8_t *b = (const uint8_t *) data;
  uint32_t c = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    c ^= b[i];
    for (int k = 0; k < 8; k++)
      c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

inline uint32_t snap_crc(const Snapshot &s) { return crc32_buf(&s, offsetof(Snapshot, crc)); }

inline void snapshot_save() {
  if (hist_part == nullptr)
    return;
  Snapshot &s = snap_buf();
  s.magic = SNAP_MAGIC;
  s.seq = ++snap_seq;
  s.day_end = fetch_time;
  s.week_end = week_fetch_time;
  s.ha_fetch = ha_fetch_at;
  s.lv_time = last_val_time;
  memcpy(s.lv, last_val, sizeof(s.lv));
  memcpy(s.day, series_day, sizeof(s.day));
  memcpy(s.week, series_week, sizeof(s.week));
  s.crc = snap_crc(s);
  uint32_t slots = hist_part->size / SNAP_SLOT;
  uint32_t off = (s.seq % slots) * SNAP_SLOT;
  if (esp_partition_erase_range(hist_part, off, SNAP_SLOT) != ESP_OK ||
      esp_partition_write(hist_part, off, &s, sizeof(s)) != ESP_OK) {
    ESP_LOGW(TAG, "history snapshot write failed");
    return;
  }
  snap_at = ::time(nullptr);
  ESP_LOGI(TAG, "history snapshot %u saved", s.seq);
}

inline void maybe_snapshot() {
  time_t now = ::time(nullptr);
  if (hist_part == nullptr || now < MIN_VALID_EPOCH)
    return;
  if (snap_at != 0 && now - snap_at < SNAP_INTERVAL)
    return;
  snapshot_save();
}

inline void snapshot_restore() {
  if (hist_part == nullptr)
    return;
  uint32_t slots = hist_part->size / SNAP_SLOT;
  uint32_t best_seq = 0;
  long best = -1;
  for (uint32_t i = 0; i < slots; i++) {
    uint32_t hdr[2];
    if (esp_partition_read(hist_part, i * SNAP_SLOT, hdr, sizeof(hdr)) != ESP_OK)
      continue;
    if (hdr[0] == SNAP_MAGIC && hdr[1] >= best_seq) {
      best_seq = hdr[1];
      best = (long) i;
    }
  }
  if (best < 0)
    return;
  Snapshot &s = snap_buf();
  if (esp_partition_read(hist_part, (uint32_t) best * SNAP_SLOT, &s, sizeof(s)) != ESP_OK)
    return;
  if (s.magic != SNAP_MAGIC || s.crc != snap_crc(s)) {
    ESP_LOGW(TAG, "history snapshot %u corrupt, ignored", s.seq);
    return;
  }
  fetch_time = (time_t) s.day_end;
  week_fetch_time = (time_t) s.week_end;
  ha_fetch_at = (time_t) s.ha_fetch;
  last_val_time = (time_t) s.lv_time;
  memcpy(last_val, s.lv, sizeof(s.lv));
  memcpy(series_day, s.day, sizeof(s.day));
  memcpy(series_week, s.week, sizeof(s.week));
  snap_seq = s.seq;
  ESP_LOGI(TAG, "history snapshot %u restored", s.seq);
}

// cold = not a deep-sleep wake: RTC memory is empty, restore from flash
inline void hist_init(bool cold) {
  hist_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "hist");
  if (hist_part == nullptr)
    ESP_LOGW(TAG, "hist partition not found, snapshots disabled");
  else if (cold)
    snapshot_restore();
}

// --- SD card: mount + tzdata override ----------------------------------------
// The card sits on the display's SPI bus (FAT32 via the Arduino SD library).
// Pins mirror the yaml spi:/detect config.

constexpr int SD_SCK = 7, SD_MISO = 8, SD_MOSI = 9, SD_CS = 14;

static bool sd_mounted = false;
static bool sd_error = false;         // last processing attempt failed
static bool sd_time_was_set = false;  // set_time applied; RTC write pending
static std::string pending_changes;   // key=value change-set awaiting apply

inline bool sd_mount() {
  if (sd_mounted)
    return true;
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS)) {
    ESP_LOGW(TAG, "SD mount failed");
    return false;
  }
  sd_mounted = true;
  ESP_LOGI(TAG, "SD mounted");
  return true;
}

// /tzdata.csv on the card overrides the embedded IANA table (data file, not
// config; regenerate with gen_tzdata.py). Loaded once per boot.
static char *tz_ovr_buf = nullptr;
static std::vector<TzEntry> tz_ovr;

inline void sd_load_tzdata() {
  if (tz_ovr_buf != nullptr)
    return;
  File f = SD.open("/tzdata.csv");
  if (!f)
    return;
  size_t sz = f.size();
  if (sz == 0 || sz > 64 * 1024) {
    f.close();
    return;
  }
  tz_ovr_buf = (char *) malloc(sz + 1);
  if (tz_ovr_buf == nullptr) {
    f.close();
    return;
  }
  f.read((uint8_t *) tz_ovr_buf, sz);
  f.close();
  tz_ovr_buf[sz] = 0;
  char *p = tz_ovr_buf;
  while (*p) {
    char *name = p;
    char *comma = strchr(p, ',');
    char *nl = strchr(p, '\n');
    if (nl == nullptr)
      nl = p + strlen(p);
    if (comma != nullptr && comma < nl) {
      *comma = 0;
      char *posix = comma + 1;
      if (*nl)
        *nl = 0;
      if (nl > posix && nl[-1] == '\r')
        nl[-1] = 0;
      tz_ovr.push_back({name, posix});
    }
    p = nl + (*nl == 0 ? 0 : 1);
    if (*nl == 0)
      break;
  }
  ESP_LOGI(TAG, "tzdata override: %d zones from SD", (int) tz_ovr.size());
}

// --- World clock zones --------------------------------------------------------

// Newlib's POSIX TZ handling on the ESP32 is unreliable (per-row setenv/tzset
// rendered every zone in the system timezone), so offsets are computed
// explicitly. All DST rules are "week'th day-of-week of month at local hour".
struct Zone {
  char city[16];
  int std_min;  // standard offset from UTC, minutes
  int dst_min;  // == std_min when the zone has no DST
  int start_month, start_week, start_dow, start_hour;  // DST begins
  int end_month, end_week, end_dow, end_hour;          // DST ends (dow 0 = Sunday)
};

// Runtime zone list, rebuilt from the HA "Zone n" text entities (and later
// from the SD config): data, not code.
constexpr int MAX_ZONES = 5;
static Zone zones[MAX_ZONES];
static int zone_count = 0;
static int home_zone = 0;  // highlighted and the base for displayed offsets

// Analog-page night window, from the HA Night From/To numbers.
static int night_from = 18, night_to = 6;

inline bool is_night(int hour) {
  return night_from <= night_to ? (hour >= night_from && hour < night_to)
                                : (hour >= night_from || hour < night_to);
}

inline bool parse_rule(const char *s, int &month, int &week, int &hour) {
  int m, w, h;
  if (sscanf(s, "%d.%d/%d", &m, &w, &h) != 3)
    return false;
  if (m < 1 || m > 12 || w < 1 || w > 5 || h < 0 || h > 23)
    return false;
  month = m;
  week = w;
  hour = h;
  return true;
}

// --- IANA name support: parse the POSIX rule strings from tzdata.h ----------

inline const char *tz_lookup(const char *name) {
  for (const auto &e : tz_ovr)  // the SD data file wins over the embedded table
    if (strcasecmp(e.name, name) == 0)
      return e.posix;
  for (int i = 0; i < TZDATA_COUNT; i++)
    if (strcasecmp(TZDATA[i].name, name) == 0)
      return TZDATA[i].posix;
  return nullptr;
}

// "America/Mexico_City" -> "Mexico City"
inline void tz_label(const char *name, char *out, size_t n) {
  const char *base = strrchr(name, '/');
  snprintf(out, n, "%s", base ? base + 1 : name);
  for (char *c = out; *c; c++)
    if (*c == '_')
      *c = ' ';
}

// "[+|-]h[h][:mm[:ss]]"; POSIX offsets are positive west, we store east.
inline bool parse_tz_offset(const char *&p, int &min_out) {
  int sign = 1;
  if (*p == '+') {
    p++;
  } else if (*p == '-') {
    sign = -1;
    p++;
  }
  if (!isdigit((unsigned char) *p))
    return false;
  int h = 0, m = 0;
  while (isdigit((unsigned char) *p))
    h = h * 10 + (*p++ - '0');
  if (*p == ':') {
    p++;
    while (isdigit((unsigned char) *p))
      m = m * 10 + (*p++ - '0');
  }
  if (*p == ':') {  // seconds: parse and ignore
    p++;
    while (isdigit((unsigned char) *p))
      p++;
  }
  if (h > 24 || m > 59)
    return false;
  min_out = -sign * (h * 60 + m);
  return true;
}

inline void skip_tz_name(const char *&p) {
  if (*p == '<') {
    while (*p && *p != '>')
      p++;
    if (*p)
      p++;
  } else {
    while (isalpha((unsigned char) *p))
      p++;
  }
}

// "Mm.w.d[/time]"; time defaults to 02:00 and may be negative or > 24.
inline bool parse_tz_rule(const char *&p, int &month, int &week, int &dow, int &hour) {
  if (*p != 'M')
    return false;
  p++;
  char *e;
  long m = strtol(p, &e, 10);
  p = e;
  if (*p++ != '.')
    return false;
  long w = strtol(p, &e, 10);
  p = e;
  if (*p++ != '.')
    return false;
  long d = strtol(p, &e, 10);
  p = e;
  long h = 2;
  if (*p == '/') {
    p++;
    h = strtol(p, &e, 10);
    p = e;
    if (*p == ':') {  // rule minutes: parse and ignore
      p++;
      strtol(p, &e, 10);
      p = e;
    }
  }
  if (m < 1 || m > 12 || w < 1 || w > 5 || d < 0 || d > 6 || h < -167 || h > 167)
    return false;
  month = (int) m;
  week = (int) w;
  dow = (int) d;
  hour = (int) h;
  return true;
}

// TZif footer form, e.g. "CET-1CEST,M3.5.0,M10.5.0/3" or "<+0330>-3:30".
inline bool zone_from_posix(const char *label, const char *posix, Zone &z) {
  const char *p = posix;
  skip_tz_name(p);
  int std_min;
  if (!parse_tz_offset(p, std_min))
    return false;
  memset(&z, 0, sizeof(z));
  snprintf(z.city, sizeof(z.city), "%s", label);
  z.std_min = std_min;
  z.dst_min = std_min;
  if (*p == 0)
    return true;  // fixed zone
  skip_tz_name(p);
  int dst_min = std_min + 60;  // POSIX default: DST is std + 1 h
  if (*p != ',' && *p != 0) {
    if (!parse_tz_offset(p, dst_min))
      return false;
  }
  if (*p++ != ',')
    return false;
  if (!parse_tz_rule(p, z.start_month, z.start_week, z.start_dow, z.start_hour))
    return false;
  if (*p++ != ',')
    return false;
  if (!parse_tz_rule(p, z.end_month, z.end_week, z.end_dow, z.end_hour))
    return false;
  z.dst_min = dst_min;
  return true;
}

// Zone spec forms:
//   "Asia/Tehran"                       IANA name from the embedded table
//   "Memphis=America/Chicago"           IANA name with a custom label
//   "City|std_min"                      fixed offset, minutes east of UTC
//   "City|std_min|dst_min|m.w/h|m.w/h"  manual DST rules (Sundays, week 5 = last)
inline bool parse_zone(const char *s, Zone &z) {
  if (strchr(s, '|') == nullptr) {
    const char *eq = strchr(s, '=');
    const char *name = eq ? eq + 1 : s;
    const char *posix = tz_lookup(name);
    if (posix == nullptr)
      return false;
    char label[16];
    if (eq) {
      size_t n = (size_t) (eq - s);
      if (n == 0 || n >= sizeof(label))
        return false;
      memcpy(label, s, n);
      label[n] = 0;
    } else {
      tz_label(name, label, sizeof(label));
    }
    return zone_from_posix(label, posix, z);
  }
  char buf[64];
  snprintf(buf, sizeof(buf), "%s", s);
  char *parts[5] = {nullptr};
  int n = 0;
  char *tok = buf;
  for (char *p = buf;; p++) {
    if (*p == '|' || *p == 0) {
      bool end = *p == 0;
      *p = 0;
      if (n < 5)
        parts[n++] = tok;
      tok = p + 1;
      if (end)
        break;
    }
  }
  if (n != 2 && n != 5)
    return false;
  if (parts[0][0] == 0 || strlen(parts[0]) >= sizeof(z.city))
    return false;
  char *endp;
  long std_v = strtol(parts[1], &endp, 10);
  if (endp == parts[1] || *endp != 0 || std_v < -720 || std_v > 840)
    return false;
  memset(&z, 0, sizeof(z));
  snprintf(z.city, sizeof(z.city), "%s", parts[0]);
  z.std_min = (int) std_v;
  z.dst_min = (int) std_v;
  if (n == 5) {
    long dst_v = strtol(parts[2], &endp, 10);
    if (endp == parts[2] || *endp != 0 || dst_v < -720 || dst_v > 840)
      return false;
    if (!parse_rule(parts[3], z.start_month, z.start_week, z.start_hour))
      return false;
    if (!parse_rule(parts[4], z.end_month, z.end_week, z.end_hour))
      return false;
    z.dst_min = (int) dst_v;
  }
  return true;
}

// Empty specs are skipped, invalid ones logged and skipped; the home city is
// matched by name and falls back to the first zone.
inline void rebuild_zones(const char *z1, const char *z2, const char *z3, const char *z4,
                          const char *z5, const char *home_city) {
  const char *in[MAX_ZONES] = {z1, z2, z3, z4, z5};
  zone_count = 0;
  for (int i = 0; i < MAX_ZONES; i++) {
    if (in[i] == nullptr || in[i][0] == 0)
      continue;
    Zone z;
    if (parse_zone(in[i], z))
      zones[zone_count++] = z;
    else
      ESP_LOGW(TAG, "invalid zone spec: %s", in[i]);
  }
  home_zone = 0;
  for (int i = 0; i < zone_count; i++)
    if (strcasecmp(zones[i].city, home_city) == 0)
      home_zone = i;
}

// --- SD config.json: validate, convert to the change-set, back up ------------

// Dated copy of the applied config; keep the newest 5.
inline void sd_backup(const String &body) {
  time_t now = ::time(nullptr);
  if (now < MIN_VALID_EPOCH)
    return;
  struct tm lt;
  localtime_r(&now, &lt);
  char name[32];
  strftime(name, sizeof(name), "/%Y-%m-%d.config.json", &lt);
  SD.remove(name);
  File b = SD.open(name, FILE_WRITE);
  if (!b) {
    ESP_LOGW(TAG, "SD backup failed");
    return;
  }
  b.print(body);
  b.close();
  char names[16][28];
  int n = 0;
  File root = SD.open("/");
  for (File e = root.openNextFile(); e && n < 16; e = root.openNextFile()) {
    const char *fn = e.name();
    size_t l = strlen(fn);
    if (l > 12 && l < 27 && strcmp(fn + l - 12, ".config.json") == 0 &&
        strcasecmp(fn, "config.json") != 0)
      snprintf(names[n++], sizeof(names[0]), "%s", fn);
    e.close();
  }
  root.close();
  for (int i = 1; i < n; i++)  // names sort chronologically
    for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--)
      std::swap(names[j - 1], names[j]);
  for (int i = 0; i < n - 5; i++) {
    char path[32];
    snprintf(path, sizeof(path), "/%s", names[i]);
    SD.remove(path);
  }
}

// Reads /config.json when its content changed since the last applied one;
// validates everything, applies set_time, and returns the change-set for the
// shared apply path ("" = nothing to do; sd_error = file rejected).
inline std::string sd_process() {
  sd_error = false;
  if (!sd_mount())
    return "";
  sd_load_tzdata();
  File f = SD.open("/config.json");
  if (!f)
    return "";
  String body = f.readString();
  f.close();
  if (body.length() == 0 || body.length() > 8192) {
    sd_error = true;
    return "";
  }
  uint32_t crc = crc32_buf(body.c_str(), body.length());
  auto pref = esphome::global_preferences->make_preference<uint32_t>(0x53444346);
  uint32_t last = 0;
  pref.load(&last);
  if (crc == last)
    return "";

  DynamicJsonDocument doc(16384);
  if (deserializeJson(doc, body) != DeserializationError::Ok || (int) (doc["version"] | 0) != 1) {
    ESP_LOGW(TAG, "SD config: invalid JSON or version");
    sd_error = true;
    return "";
  }

  std::string ch;
  auto add = [&](const char *kv) {
    if (!ch.empty())
      ch += ';';
    ch += kv;
  };

  if (doc["zones"].is<JsonArray>()) {
    JsonArray za = doc["zones"].as<JsonArray>();
    if (za.size() < 1 || za.size() > (size_t) MAX_ZONES) {
      sd_error = true;
      return "";
    }
    int zi = 0;
    for (JsonObject z : za) {
      char spec[64];
      if (z["tz"].is<const char *>()) {
        const char *label = z["label"] | "";
        if (label[0])
          snprintf(spec, sizeof(spec), "%s=%s", label, (const char *) z["tz"]);
        else
          snprintf(spec, sizeof(spec), "%s", (const char *) z["tz"]);
      } else if (z["dst_start"].is<JsonObject>()) {
        snprintf(spec, sizeof(spec), "%s|%d|%d|%d.%d/%d|%d.%d/%d",
                 (const char *) (z["city"] | ""), (int) (z["std_offset_min"] | 0),
                 (int) (z["dst_offset_min"] | 0), (int) z["dst_start"]["month"],
                 (int) z["dst_start"]["week"], (int) z["dst_start"]["hour"],
                 (int) z["dst_end"]["month"], (int) z["dst_end"]["week"],
                 (int) z["dst_end"]["hour"]);
      } else {
        snprintf(spec, sizeof(spec), "%s|%d", (const char *) (z["city"] | ""),
                 (int) (z["std_offset_min"] | 0));
      }
      Zone tmp;
      if (!parse_zone(spec, tmp)) {
        ESP_LOGW(TAG, "SD config: bad zone '%s'", spec);
        sd_error = true;
        return "";
      }
      char kv[80];
      snprintf(kv, sizeof(kv), "zone%d=%s", ++zi, spec);
      add(kv);
    }
    while (zi < MAX_ZONES) {
      char kv[12];
      snprintf(kv, sizeof(kv), "zone%d=", ++zi);
      add(kv);
    }
  }

  if (doc["columns"].is<JsonArray>()) {
    JsonArray ca = doc["columns"].as<JsonArray>();
    if (ca.size() < 1 || ca.size() > (size_t) MAX_COLS) {
      sd_error = true;
      return "";
    }
    int ci = 0;
    for (JsonObject c : ca) {
      char spec[144];
      snprintf(spec, sizeof(spec), "%s=%s", (const char *) (c["label"] | ""),
               (const char *) (c["source"] | ""));
      Column tmp{};
      if (!parse_column(spec, tmp)) {
        ESP_LOGW(TAG, "SD config: bad column '%s'", spec);
        sd_error = true;
        return "";
      }
      char kv[160];
      snprintf(kv, sizeof(kv), "col%d=%s", ++ci, spec);
      add(kv);
    }
    while (ci < MAX_COLS) {
      char kv[8];
      snprintf(kv, sizeof(kv), "col%d=", ++ci);
      add(kv);
    }
  }

  char kv[64];
  if (doc["home_zone"].is<const char *>()) {
    snprintf(kv, sizeof(kv), "home=%s", (const char *) doc["home_zone"]);
    add(kv);
  }
  if (doc["night_from"].is<int>() && doc["night_to"].is<int>()) {
    snprintf(kv, sizeof(kv), "night=%d-%d", (int) doc["night_from"], (int) doc["night_to"]);
    add(kv);
  }
  if (doc["start_page"].is<int>()) {
    snprintf(kv, sizeof(kv), "start=%d", (int) doc["start_page"]);
    add(kv);
  }
  if (doc["sync_interval_min"].is<int>()) {
    snprintf(kv, sizeof(kv), "sync=%d", (int) doc["sync_interval_min"]);
    add(kv);
  }
  if (doc["show_pages"].is<JsonArray>()) {
    JsonArray sp = doc["show_pages"].as<JsonArray>();
    for (int i = 0; i < PAGE_COUNT && i < (int) sp.size(); i++) {
      snprintf(kv, sizeof(kv), "show%d=%s", i + 1, sp[i].as<bool>() ? "on" : "off");
      add(kv);
    }
  }

  // one-shot air-gapped clock set: applied only when the value changes
  if (doc["set_time"].is<const char *>()) {
    time_t ts = parse_iso_utc(doc["set_time"]);
    auto tp = esphome::global_preferences->make_preference<int64_t>(0x53445354);
    int64_t last_ts = 0;
    tp.load(&last_ts);
    if (ts > 0 && (int64_t) ts != last_ts) {
      struct timeval tv = {.tv_sec = ts, .tv_usec = 0};
      settimeofday(&tv, nullptr);
      last_ts = (int64_t) ts;
      tp.save(&last_ts);
      sd_time_was_set = true;
      ESP_LOGI(TAG, "SD config: clock set from set_time");
    }
  }

  pref.save(&crc);
  esphome::global_preferences->sync();
  sd_backup(body);
  ESP_LOGI(TAG, "SD config accepted (%d bytes)", (int) body.length());
  return ch;
}

// UTC epoch of the week'th dow (week 5 = last) of month at a local hour;
// offset_min is the zone offset in effect before the transition.
inline time_t transition_epoch(int year, int month, int week, int dow, int hour,
                               int offset_min) {
  static const int MDAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  long first = days_from_civil(year, month, 1);
  int dim = MDAYS[month - 1];
  if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
    dim++;
  int day;
  if (week == 5) {
    int last_dow = (int) ((first + dim - 1 + 4) % 7);  // 0 = Sunday
    day = dim - (last_dow - dow + 7) % 7;
  } else {
    int first_dow = (int) ((first + 4) % 7);
    day = 1 + (dow - first_dow + 7) % 7 + (week - 1) * 7;
    if (day > dim)
      day -= 7;
  }
  return (time_t) days_from_civil(year, month, day) * 86400 + hour * 3600 - offset_min * 60;
}

inline int zone_offset_min(const Zone &z, time_t t) {
  if (z.dst_min == z.std_min)
    return z.std_min;
  struct tm g;
  gmtime_r(&t, &g);
  int year = g.tm_year + 1900;
  time_t dst_start =
      transition_epoch(year, z.start_month, z.start_week, z.start_dow, z.start_hour, z.std_min);
  time_t dst_end =
      transition_epoch(year, z.end_month, z.end_week, z.end_dow, z.end_hour, z.dst_min);
  // start > end = southern hemisphere (or Ireland-style negative DST): the
  // DST period wraps the new year
  bool in_dst = dst_start <= dst_end ? (t >= dst_start && t < dst_end)
                                     : (t >= dst_start || t < dst_end);
  return in_dst ? z.dst_min : z.std_min;
}

// Offset of the system timezone (installed by HA) in minutes, derived by
// comparing the local and UTC calendars.
inline int local_offset_min(time_t t) {
  struct tm lt, gt;
  localtime_r(&t, &lt);
  gmtime_r(&t, &gt);
  long days = days_from_civil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday) -
              days_from_civil(gt.tm_year + 1900, gt.tm_mon + 1, gt.tm_mday);
  return (int) (days * 1440 + (lt.tm_hour - gt.tm_hour) * 60 + (lt.tm_min - gt.tm_min));
}

// --- Pages ----------------------------------------------------------------

// Small sun (day) / crescent moon (night) glyph; bg carves the crescent so
// it works on normal and inverted rows alike.
inline void draw_day_night(Display &it, int cx, int cy, bool night, esphome::Color fg,
                           esphome::Color bg) {
  if (night) {
    it.filled_circle(cx, cy, 9, fg);
    it.filled_circle(cx + 5, cy - 3, 8, bg);
  } else {
    it.filled_circle(cx, cy, 5, fg);
    for (int k = 0; k < 8; k++) {
      float a = k * 3.14159265f / 4.0f;
      it.line(cx + (int) (cosf(a) * 7.0f), cy + (int) (sinf(a) * 7.0f),
              cx + (int) (cosf(a) * 11.0f), cy + (int) (sinf(a) * 11.0f), fg);
    }
  }
}

inline void draw_world_clock(Display &it, BaseFont *big, BaseFont *med, BaseFont *small_f) {
  time_t now = ::time(nullptr);
  it.print(400, 12, med, TextAlign::TOP_CENTER, "World Clock");
  it.line(20, 55, 780, 55);
  if (now < MIN_VALID_EPOCH) {
    it.print(400, 220, med, TextAlign::CENTER, "Waiting for time sync...");
    return;
  }
  if (zone_count == 0) {
    it.print(400, 220, med, TextAlign::CENTER, "No zones configured");
    return;
  }
  int base_off = zone_offset_min(zones[home_zone], now);
  int y = 75 + (MAX_ZONES - zone_count) * 40;  // vertically centered
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
    if (is_local)
      it.filled_rectangle(0, y - 10, 800, 66);
    it.print(40, y, big, fg, TextAlign::TOP_LEFT, z.city);
    if (!is_local) {
      int rel = off - base_off;
      int a = rel < 0 ? -rel : rel;
      it.printf(430, y + 24, small_f, fg, TextAlign::TOP_RIGHT, "%c%d:%02d",
                rel < 0 ? '-' : '+', a / 60, a % 60);
    }
    it.print(580, y + 24, small_f, fg, TextAlign::TOP_RIGHT, date);
    it.print(725, y, big, fg, TextAlign::TOP_RIGHT, hhmm);
    draw_day_night(it, 762, y + 26, is_night(lt.tm_hour), fg,
                   is_local ? esphome::display::COLOR_ON : esphome::display::COLOR_OFF);
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
                            float dev_h, bool show_climate, bool sd_present, bool show_jalali) {
  it.filled_rectangle(0, 446, 800, 34);
  time_t now = ::time(nullptr);
  if (now > MIN_VALID_EPOCH) {
    struct tm lt;
    localtime_r(&now, &lt);
    char greg[24];
    strftime(greg, sizeof(greg), "%a %d %b %Y", &lt);
    if (show_jalali) {
      int jy, jm, jd;
      jalali::from_gregorian(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, jy, jm, jd);
      it.printf(20, 463, small_f, esphome::display::COLOR_OFF, TextAlign::CENTER_LEFT,
                "%d %s %d", jd, jalali::MONTHS[jm - 1], jy);
    }
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
    if (std::isnan(t))
      it.print(x, 140, large, TextAlign::TOP_CENTER, "--");
    else
      it.printf(x, 130, huge, TextAlign::TOP_CENTER, "%.1f°C", t);
    if (std::isnan(h))
      it.print(x, 290, large, TextAlign::TOP_CENTER, "--");
    else
      it.printf(x, 280, huge, TextAlign::TOP_CENTER, "%.0f%%", h);
  }
}

// Series distinction on a 1-bit panel, papers-style: line weight + dash
// pattern + a distinct point marker per series (circle / square / triangle).
inline bool style_skip(int style, int i) {
  if (style == 1)
    return (i % 4) >= 2;
  if (style == 2)
    return (i % 6) >= 2;
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
// no values — those live on the numbers page. Items flow left to right with
// a fixed gap; labels are measured so spacing stays even.
inline void draw_legend(Display &it, BaseFont *f, int x, int y) {
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
  draw_legend(it, tiny_f, 330, 10);
  draw_graph_band(it, series_day, metric, fetch_time, SPAN, med, tiny_f, 48, 420, true);
}

// Combined 7-day page: temperature and humidity stacked, one legend, shared
// weekday axis at the bottom, vertical inverted section titles.
inline void draw_week_page(Display &it, BaseFont *med, BaseFont *small_f, BaseFont *tiny_f) {
  if (no_cols(it, med))
    return;
  if (any_ha_col() && ha_fetch_at > MIN_VALID_EPOCH) {
    struct tm lt;
    localtime_r(&ha_fetch_at, &lt);
    it.printf(6, 6, tiny_f, TextAlign::TOP_LEFT, "data %02d:%02d", lt.tm_hour, lt.tm_min);
  }
  draw_legend(it, tiny_f, 110, 6);

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
    draw_graph_band(it, series_week, band.metric, week_fetch_time, SPAN_WEEK, med, tiny_f,
                    band.y0, band.y1, false);
  }

  // weekday labels centered per day, once, under the bottom band
  if (week_fetch_time > MIN_VALID_EPOCH) {
    const int X0 = 80, X1 = 784;
    int loff = local_offset_min(week_fetch_time);
    time_t lstart = week_fetch_time - SPAN_WEEK + (time_t) loff * 60;
    time_t midnight0 = lstart - (lstart % 86400) + 86400;
    for (time_t lm = midnight0 - 86400; lm < lstart + SPAN_WEEK; lm += 86400) {
      time_t center = lm + 43200;
      if (center < lstart || center > lstart + SPAN_WEEK)
        continue;
      int x = X0 + (int) ((center - lstart) * (long) (X1 - X0) / SPAN_WEEK);
      struct tm dt;
      gmtime_r(&center, &dt);
      char day[8];
      strftime(day, sizeof(day), "%a", &dt);
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
inline void draw_analog_clocks(Display &it, BaseFont *med, BaseFont *small_f) {
  time_t now = ::time(nullptr);
  if (now < MIN_VALID_EPOCH) {
    it.print(400, 220, med, TextAlign::CENTER, "Waiting for time sync...");
    return;
  }
  constexpr float PI_F = 3.14159265f;
  // dial positions per zone count (1..5)
  static const struct {
    int cx, cy;
  } LAYOUTS[MAX_ZONES][MAX_ZONES] = {
      {{400, 220}},
      {{266, 220}, {533, 220}},
      {{133, 220}, {400, 220}, {667, 220}},
      {{266, 124}, {533, 124}, {266, 318}, {533, 318}},
      {{133, 124}, {400, 124}, {667, 124}, {266, 318}, {533, 318}},
  };
  const int R = 96;
  if (zone_count == 0) {
    it.print(400, 220, med, TextAlign::CENTER, "No zones configured");
    return;
  }
  const auto *pos = LAYOUTS[zone_count - 1];
  int base_off = zone_offset_min(zones[home_zone], now);
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
