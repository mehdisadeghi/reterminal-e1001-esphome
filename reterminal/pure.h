#pragma once

// Pure logic: parsers, calendars, timezone math. No Arduino/ESP dependencies
// so it compiles host-side for the unit tests (tests/host_test.cpp).

#include <strings.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#if defined(RETERMINAL_HOST_TEST)
#define RT_LOGW(tag, ...) ((void) 0)
#define RT_LOGI(tag, ...) ((void) 0)
#else
#include "esphome/core/log.h"
#define RT_LOGW ESP_LOGW
#define RT_LOGI ESP_LOGI
#endif

#include "tzdata.h"

namespace reterminal {

static const char *const TAG = "reterminal";

constexpr int PAGE_COUNT = 8;  // clock, numbers, temp, hum, week, analog, noqte, khayyam
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
      RT_LOGW(TAG, "invalid column spec: %s", in[i]);
    }
  }
}

inline bool any_ha_col() {
  for (int i = 0; i < MAX_COLS; i++)
    if (cols[i].enabled && !cols[i].is_dev)
      return true;
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


// SD-loaded override of the embedded tz table (loader lives in device.h).
static char *tz_ovr_buf = nullptr;
static std::vector<TzEntry> tz_ovr;

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
      RT_LOGW(TAG, "invalid zone spec: %s", in[i]);
  }
  home_zone = 0;
  for (int i = 0; i < zone_count; i++)
    if (strcasecmp(zones[i].city, home_city) == 0)
      home_zone = i;
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

}  // namespace reterminal
