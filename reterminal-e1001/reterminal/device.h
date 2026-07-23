#pragma once

// Device-side state and I/O: RTC memory, sleep bookkeeping, HA REST access,
// flash snapshots, and the SD card stack.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <SD.h>
#include <SPI.h>
#include <sys/time.h>
#include <string>
#include "esp_attr.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_sleep.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "pure.h"

namespace reterminal {

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
RTC_DATA_ATTR int week_days = 7;           // combo page span in days
RTC_DATA_ATTR time_t ha_fetch_at = 0;      // last successful HA history fetch
RTC_DATA_ATTR int16_t series_day[MAX_COLS][2][POINTS];
RTC_DATA_ATTR int16_t series_week[MAX_COLS][2][POINTS];
RTC_DATA_ATTR float last_val[MAX_COLS][2] = {{NAN, NAN}, {NAN, NAN}, {NAN, NAN}};
RTC_DATA_ATTR time_t last_val_time = 0;
RTC_DATA_ATTR time_t ha_live_at = 0;  // last successful /api/states poll
RTC_DATA_ATTR time_t beep_ack = 0;  // last handled "single beep" request
RTC_DATA_ATTR time_t sync_attempt_at = 0;

inline time_t week_span() { return (time_t) week_days * 86400; }

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

// Display cadence: one config drives both the deep-sleep wake interval and
// the redraw interval while the device is held awake (keep-awake / pause).
static int refresh_interval_min = 3;
static int night_refresh_min = 0;  // 0 = no night slowdown

// Wi-Fi strike-out: three consecutive wakes ending without a connection
// stop further attempts. The counter lives in RTC memory, so deep sleep
// keeps it and any real reboot starts fresh.
RTC_DATA_ATTR int wifi_fail_wakes = 0;
constexpr int WIFI_FAIL_LIMIT = 3;
inline bool wifi_strikeout() { return wifi_fail_wakes >= WIFI_FAIL_LIMIT; }

// Sleep length in minutes: the refresh interval, stretched to the night
// interval while the home zone's local time is inside the night window
// (no home zone = no night slowdown).
inline int sleep_minutes() {
  if (night_refresh_min > 0 && home_zone >= 0 && home_zone < zone_count) {
    time_t now = ::time(nullptr);
    if (now > MIN_VALID_EPOCH) {
      time_t local = now + (time_t) zone_offset_min(zones[home_zone], now) * 60;
      struct tm lt;
      gmtime_r(&local, &lt);
      if (is_night(lt.tm_hour))
        return night_refresh_min;
    }
  }
  return refresh_interval_min;
}
static uint32_t last_draw_ms = 0;

inline void mark_draw() { last_draw_ms = millis(); }
inline bool redraw_due() {
  return millis() - last_draw_ms >= (uint32_t) refresh_interval_min * 60000;
}

// Mirrors of HA entities and boot diagnostics, set from YAML.
static bool radio_on = true;      // wifi enabled this wake
static bool debug_page = false;   // transient hardware/config info page
static bool rtc_bad = false;      // RTC chip unusable at boot (dead cell / VL / frozen)
static time_t pre_rtc_time = 0;   // system time captured before the RTC read
static bool force_sync = false;  // green button: sync + fetch now
static int sync_interval_min = 15;
static bool page_enabled_[PAGE_COUNT] = {true, true, true, true, true, true, true, true};
static bool bar_on_[PAGE_COUNT] = {true, true, true, true, true, true, true, true};

// The status bar shows on every page except the listed ones; "7,8" =
// hidden on pages 7 and 8 (1-based, comma-separated, empty = everywhere)
inline void set_bar_skip(const char *s) {
  for (int i = 0; i < PAGE_COUNT; i++)
    bar_on_[i] = true;
  while (*s) {
    const char *e = strchr(s, ',');
    size_t len = e ? (size_t) (e - s) : strlen(s);
    if (len == 1 && s[0] >= '1' && s[0] < '1' + PAGE_COUNT)
      bar_on_[s[0] - '1'] = false;
    else if (len)
      ESP_LOGW(TAG, "bar skip: bad token '%.*s'", (int) len, s);
    s += len + (e ? 1 : 0);
  }
}

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

// page indices match the display lambda: 2 numbers, 3-5 graphs
inline bool page_needs_live(int p) { return p == 2 && any_ha_col(); }
inline bool page_needs_history(int p) { return p >= 3 && p <= 5 && any_ha_col(); }

// True when no enabled HA column holds a single valid point — dev columns
// refill themselves per wake and don't count.
inline bool ha_series_empty(const int16_t rows[MAX_COLS][2][POINTS]) {
  bool any = false;
  for (int c = 0; c < MAX_COLS; c++) {
    if (!cols[c].enabled || cols[c].is_dev)
      continue;
    any = true;
    for (int m = 0; m < 2; m++)
      for (int i = 0; i < POINTS; i++)
        if (rows[c][m][i] != PT_NAN)
          return false;
  }
  return any;
}

// A data page showing missing or stale HA values earns a refresh — on
// button flips and on wakes alike (a rolled-empty window would otherwise
// stay blank until the 3 h fetch interval expires).
inline bool page_data_empty(int p) {
  if (p == 2) {
    time_t now = ::time(nullptr);
    return page_needs_live(p) && (ha_live_at == 0 || now - ha_live_at > 600);
  }
  if (p == 3 || p == 4)
    return page_needs_history(p) && (ha_fetch_at == 0 || ha_series_empty(series_day));
  if (p == 5)
    return page_needs_history(p) && (ha_fetch_at == 0 || ha_series_empty(series_week));
  return false;
}

inline bool network_needed() {
  return sync_due() || page_needs_live(page) ||
         (page_needs_history(page) && (fetch_due() || page_data_empty(page)));
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
  roll_rows(series_week, week_fetch_time, week_span() / POINTS, now);
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
  roll_rows(series_week, week_fetch_time, week_span() / POINTS, end);
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
      if (fetch_entity(base, token, ent[m], end - week_span(), week_span() / POINTS, row))
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

// One-shot by default; pass the previous return value as crc to stream.
inline uint32_t crc32_buf(const void *data, size_t len, uint32_t crc = 0) {
  const uint8_t *b = (const uint8_t *) data;
  uint32_t c = ~crc;
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

// Validates a config.json body when its content changed since the last
// applied one, applies set_time, and returns the change-set for the shared
// apply path ("" = nothing to do; sd_error = body rejected). Shared by the
// SD card and the remote CONFIG_URL — only the transport differs.
inline std::string process_config(const String &body, bool from_sd) {
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
  if (doc["refresh_interval_min"].is<int>()) {
    snprintf(kv, sizeof(kv), "refresh=%d", (int) doc["refresh_interval_min"]);
    add(kv);
  }
  if (doc["night_refresh_min"].is<int>()) {
    snprintf(kv, sizeof(kv), "nrefresh=%d", (int) doc["night_refresh_min"]);
    add(kv);
  }
  if (doc["lang"].is<const char *>()) {
    snprintf(kv, sizeof(kv), "lang=%s", (const char *) doc["lang"]);
    add(kv);
  }
  if (doc["combo_days"].is<int>()) {
    snprintf(kv, sizeof(kv), "days=%d", (int) doc["combo_days"]);
    add(kv);
  }
  if (doc["show_pages"].is<JsonArray>()) {
    JsonArray sp = doc["show_pages"].as<JsonArray>();
    for (int i = 0; i < PAGE_COUNT && i < (int) sp.size(); i++) {
      snprintf(kv, sizeof(kv), "show%d=%s", i + 1, sp[i].as<bool>() ? "on" : "off");
      add(kv);
    }
  }
  if (doc["bar_skip_pages"].is<const char *>()) {
    snprintf(kv, sizeof(kv), "barskip=%s", (const char *) doc["bar_skip_pages"]);
    add(kv);
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
  if (from_sd)
    sd_backup(body);
  ESP_LOGI(TAG, "config accepted (%d bytes)", (int) body.length());
  return ch;
}

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
  return process_config(body, true);
}

// --- Remote config: CONFIG_URL over HTTP(S), no card involved ----------------
// Checked once per day on a network wake, or on demand (long green press).
// https is accepted without certificate pinning — the URL is the trust
// anchor, pick one you control.

RTC_DATA_ATTR time_t cfg_fetch_at = 0;
constexpr time_t CFG_FETCH_PERIOD = 24 * 3600;

inline bool remote_cfg_due() {
  time_t now = ::time(nullptr);
  return now > MIN_VALID_EPOCH && now - cfg_fetch_at >= CFG_FETCH_PERIOD;
}

inline void fetch_remote_config(const char *url) {
  if (url[0] == 0)
    return;
  sd_error = false;
  cfg_fetch_at = ::time(nullptr);  // rate-limits attempts, not successes
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(15000);
  NetworkClientSecure tls;
  bool opened;
  if (strncmp(url, "https", 5) == 0) {
    tls.setInsecure();
    opened = http.begin(tls, url);
  } else {
    opened = http.begin(url);
  }
  if (!opened) {
    ESP_LOGW(TAG, "remote config: bad url");
    sd_error = true;
    return;
  }
  int code = http.GET();
  if (code != 200) {
    ESP_LOGW(TAG, "remote config: HTTP %d", code);
    http.end();
    sd_error = true;
    return;
  }
  String body = http.getString();
  http.end();
  std::string ch = process_config(body, false);
  if (!ch.empty())
    pending_changes = ch;
}

// --- SD firmware update & app-level rollback ----------------------------------
//
// /firmware.bin on the card (the plain OTA app image, not *.factory.bin) is
// flashed into the passive app slot and booted. The Arduino framework ships
// a bootloader without rollback support, so the safety net is app level:
// flashing arms an NVS record, the earliest boot hook counts boot attempts
// while it is armed, and a completed boot_flow confirms the image; after
// FW_BOOT_TRIES failed attempts the previous slot boots again.

struct FwMark {  // fingerprint of the last applied /firmware.bin
  uint32_t size;
  uint32_t mtime;
  uint32_t crc;
};

struct FwPending {  // armed between flashing and a confirmed boot
  bool armed;
  uint8_t attempts;
  uint32_t prev_addr;
};

constexpr uint32_t FW_MARK_KEY = 0x46574D4B;     // 'FWMK'
constexpr uint32_t FW_PENDING_KEY = 0x46575044;  // 'FWPD'
constexpr int FW_BOOT_TRIES = 3;
static bool sd_fw_error = false;  // last flash attempt failed

inline void rollback_arm() {
  FwPending p = {true, 0, esp_ota_get_running_partition()->address};
  auto pref = esphome::global_preferences->make_preference<FwPending>(FW_PENDING_KEY);
  pref.save(&p);
  esphome::global_preferences->sync();
}

inline void rollback_confirm() {
  auto pref = esphome::global_preferences->make_preference<FwPending>(FW_PENDING_KEY);
  FwPending p{};
  if (!pref.load(&p) || !p.armed)
    return;
  p.armed = false;
  pref.save(&p);
  esphome::global_preferences->sync();
  ESP_LOGI(TAG, "firmware update confirmed");
}

// Earliest boot hook. The attempt counter must hit flash before anything
// that could crash runs, or a bootloop would never be counted.
inline void rollback_check() {
  auto pref = esphome::global_preferences->make_preference<FwPending>(FW_PENDING_KEY);
  FwPending p{};
  if (!pref.load(&p) || !p.armed)
    return;
  p.attempts++;
  pref.save(&p);
  esphome::global_preferences->sync();
  if (p.attempts < FW_BOOT_TRIES)
    return;
  p.armed = false;
  pref.save(&p);
  esphome::global_preferences->sync();
  // the esp_partition_t registry entries outlive the iterator
  const esp_partition_t *prev = nullptr;
  esp_partition_iterator_t it =
      esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  for (; it != nullptr && prev == nullptr; it = esp_partition_next(it))
    if (esp_partition_get(it)->address == p.prev_addr)
      prev = esp_partition_get(it);
  esp_partition_iterator_release(it);
  if (prev == nullptr) {
    ESP_LOGE(TAG, "rollback: previous slot not found");
    return;
  }
  ESP_LOGE(TAG, "boot never completed; rolling back to %s", prev->label);
  esp_ota_set_boot_partition(prev);
  esp_restart();
}

// True = a new image was flashed and the boot slot switched; the caller
// reboots. A size+mtime fingerprint in NVS keeps the per-wake cost at one
// stat; the content CRC decides whether to actually flash.
inline bool sd_flash_firmware() {
  sd_fw_error = false;
  if (!sd_mount())
    return false;
  File f = SD.open("/firmware.bin");
  if (!f)
    return false;
  auto pref = esphome::global_preferences->make_preference<FwMark>(FW_MARK_KEY);
  FwMark last{};
  pref.load(&last);
  FwMark cur = {(uint32_t) f.size(), (uint32_t) f.getLastWrite(), 0};
  if (cur.size == last.size && cur.mtime == last.mtime) {
    f.close();
    return false;
  }
  static uint8_t buf[4096];
  uint8_t magic = 0;
  bool first_chunk = true;
  int n;
  while ((n = f.read(buf, sizeof(buf))) > 0) {
    if (first_chunk) {
      magic = buf[0];
      first_chunk = false;
    }
    cur.crc = crc32_buf(buf, n, cur.crc);
    esphome::App.feed_wdt();
  }
  if (cur.crc == last.crc) {  // same image, touched timestamps: just remember
    pref.save(&cur);
    f.close();
    return false;
  }
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  if (next == nullptr || magic != 0xE9 || cur.size < 65536 || cur.size > next->size) {
    ESP_LOGW(TAG, "firmware.bin is not a flashable app image");
    sd_fw_error = true;
    f.close();
    return false;
  }
  ESP_LOGI(TAG, "flashing %u bytes into %s", cur.size, next->label);
  f.seek(0);
  esp_ota_handle_t h;
  if (esp_ota_begin(next, cur.size, &h) != ESP_OK) {
    ESP_LOGW(TAG, "esp_ota_begin failed");
    sd_fw_error = true;
    f.close();
    return false;
  }
  bool ok = true;
  while (ok && (n = f.read(buf, sizeof(buf))) > 0) {
    ok = esp_ota_write(h, buf, n) == ESP_OK;
    esphome::App.feed_wdt();
  }
  f.close();
  if (!ok) {
    esp_ota_abort(h);
    ESP_LOGW(TAG, "flash write failed");
    sd_fw_error = true;
    return false;
  }
  if (esp_ota_end(h) != ESP_OK) {  // full image validation happens here
    ESP_LOGW(TAG, "firmware image failed validation");
    sd_fw_error = true;
    return false;
  }
  rollback_arm();
  if (esp_ota_set_boot_partition(next) != ESP_OK) {
    rollback_confirm();
    ESP_LOGW(TAG, "could not switch the boot slot");
    sd_fw_error = true;
    return false;
  }
  pref.save(&cur);
  esphome::global_preferences->sync();
  ESP_LOGI(TAG, "firmware flashed; rebooting into %s", next->label);
  return true;
}

}  // namespace reterminal
