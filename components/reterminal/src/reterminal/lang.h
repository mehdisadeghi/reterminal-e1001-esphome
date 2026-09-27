#pragma once

// Runtime UI language. Each language is a Pack — strings, calendar tables,
// a digit mapper, and its text direction — plus a font set registered per
// language (see FontRole in draw.h). Draw code never branches on the
// language: it reads L() for strings and F(role) for fonts, both plain
// table lookups. Adding a language: generate its strings header (the
// tools/gen_fa_strings.py pattern), add its fonts to packages/fonts.yaml,
// and append one row to PACKS plus one register_fonts() argument group.
//
// Direction is inferred per context: text flows follow L().rtl, while
// things that stay LTR in any language (graph axes, dials, URLs, °C,
// technical tags, runtime-entered labels) simply never consult it.

#include <cstdio>
#include <cstring>
#include <strings.h>

#include "hijri.h"
#include "jalali.h"
#include "strings_fa.h"

namespace lang {

// ASCII passthrough (LTR scripts using Latin digits)
inline void num_ascii(char *out, size_t n, const char *in) { snprintf(out, n, "%s", in); }

// ASCII digits -> Persian digits, '.' -> momayyez; the rest copies through.
// Numbers keep their internal LTR order even inside RTL text.
inline void num_persian(char *out, size_t n, const char *in) {
  size_t o = 0;
  for (const char *p = in; *p != 0 && o + 3 < n; p++) {
    if (*p >= '0' && *p <= '9') {
      out[o++] = (char) 0xDB;
      out[o++] = (char) (0xB0 + (*p - '0'));
    } else if (*p == '.') {
      out[o++] = (char) 0xD9;
      out[o++] = (char) 0xAB;
    } else {
      out[o++] = *p;
    }
  }
  out[o] = 0;
}

namespace en {
constexpr const char *TITLE_CLOCK = "World Clock";
constexpr const char *WAIT_SYNC = "Waiting for time sync...";
constexpr const char *NO_ZONES = "No zones configured";
constexpr const char *NO_COLS = "No columns configured";
constexpr const char *TITLE_TEMP = "Temperature - 24h";
constexpr const char *TITLE_HUM = "Humidity - 24h";
constexpr const char *WIFI_TITLE = "Wi-Fi setup";
constexpr const char *WIFI_JOIN = "Join the hotspot:";
constexpr const char *OPEN_URL = "then open http://192.168.4.1";
constexpr const char *UNIT_T = "T";
constexpr const char *UNIT_H = "H";
constexpr const char *CHARGE = "Charge!";
constexpr const char *ZODIAC_LABEL = "Zodiac";
constexpr const char *CAL_IR = "Iranian lunar";
constexpr const char *CAL_TAB = "tabular lunar";
static const char *const WEEKDAYS[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const MONTHS[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const WEEKDAYS_FULL[7] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                             "Thursday", "Friday", "Saturday"};
static const char *const MONTHS_FULL[12] = {"January", "February", "March",     "April",
                                            "May",     "June",     "July",      "August",
                                            "September", "October", "November", "December"};
static const char *const ZODIAC[12] = {"Aries", "Taurus",  "Gemini",      "Cancer",
                                       "Leo",   "Virgo",   "Libra",       "Scorpio",
                                       "Sagittarius", "Capricorn", "Aquarius", "Pisces"};
}  // namespace en

struct Pack {
  const char *code;  // select option value and the SD/queue "lang" key
  bool rtl;
  // translated city for an IANA zone name, nullptr = none / use labels
  const char *(*city)(const char *iana);
  void (*num)(char *out, size_t n, const char *in);
  const char *title_clock;
  const char *wait_sync, *no_zones, *no_cols, *title_temp, *title_hum;
  const char *wifi_title, *wifi_join, *open_url, *unit_pct, *unit_t, *unit_h;
  const char *const *weekdays;  // [7], tm_wday order
  const char *const *months;    // [12] Gregorian
  const char *const *jmonths;   // [12] Solar Hijri
  const char *const *hmonths;   // [12] Lunar Hijri
  // calendar page: unabbreviated names and the zodiac (fa tables are
  // already full, so they double up)
  const char *const *wdays_full;   // [7]
  const char *const *months_full;  // [12] Gregorian
  const char *const *zodiac;       // [12], index = Jalali month - 1
  const char *zodiac_label;
  const char *cal_ir, *cal_tab;    // lunar Hijri source annotation
  const char *charge;              // empty-battery screen
};

inline const char *fa_city(const char *iana) {
  for (int i = 0; i < fa::CITY_COUNT; i++)
    if (strcasecmp(fa::CITIES[i].iana, iana) == 0)
      return fa::CITIES[i].name;
  return nullptr;
}

static const Pack PACKS[] = {
    {"en", false, nullptr, num_ascii, en::TITLE_CLOCK, en::WAIT_SYNC, en::NO_ZONES, en::NO_COLS, en::TITLE_TEMP,
     en::TITLE_HUM, en::WIFI_TITLE, en::WIFI_JOIN, en::OPEN_URL, "%", en::UNIT_T, en::UNIT_H,
     en::WEEKDAYS, en::MONTHS, jalali::MONTHS, hijri::MONTHS, en::WEEKDAYS_FULL,
     en::MONTHS_FULL, en::ZODIAC, en::ZODIAC_LABEL, en::CAL_IR, en::CAL_TAB, en::CHARGE},
    {"fa", true, fa_city, num_persian, fa::TITLE_CLOCK, fa::WAIT_SYNC, fa::NO_ZONES, fa::NO_COLS, fa::TITLE_TEMP,
     fa::TITLE_HUM, fa::WIFI_TITLE, fa::WIFI_JOIN, fa::OPEN_URL, "٪", fa::UNIT_T, fa::UNIT_H,
     fa::WEEKDAYS, fa::MONTHS, fa::JMONTHS, fa::HMONTHS, fa::WEEKDAYS, fa::MONTHS,
     fa::ZODIAC, fa::ZODIAC_LABEL, fa::CAL_IR, fa::CAL_TAB, fa::CHARGE},
};
constexpr int LANG_COUNT = sizeof(PACKS) / sizeof(PACKS[0]);

static int lang_idx = 0;

inline const Pack &L() { return PACKS[lang_idx]; }
inline bool rtl() { return L().rtl; }

inline int lang_index(const char *code) {
  for (int i = 0; i < LANG_COUNT; i++)
    if (strcmp(PACKS[i].code, code) == 0)
      return i;
  return 0;
}

// Compositions. Word order flips with direction (buffers hold visual
// order); the pack's tables and digit mapper carry everything else. A
// future language needing a third date order grows these helpers, not
// the draw code.
inline void num(char *out, size_t n, const char *in) { L().num(out, n, in); }

inline void percent(char *out, size_t n, const char *digits) {
  char d[16];
  L().num(d, sizeof(d), digits);
  if (L().rtl)
    snprintf(out, n, "%s%s", L().unit_pct, d);
  else
    snprintf(out, n, "%s%s", d, L().unit_pct);
}

// Status-bar climate readout: "T 23° H 45" / visual-order "۴۵ ر ۲۳° د"
inline void climate(char *out, size_t n, float t, float h) {
  char tb[8], hb[8], tl[16], hl[16];
  snprintf(tb, sizeof(tb), "%.0f", (double) t);
  snprintf(hb, sizeof(hb), "%.0f", (double) h);
  L().num(tl, sizeof(tl), tb);
  L().num(hl, sizeof(hl), hb);
  if (L().rtl)
    snprintf(out, n, "%s %s %s° %s", hl, L().unit_h, tl, L().unit_t);
  else
    snprintf(out, n, "%s %s° %s %s", L().unit_t, tl, L().unit_h, hl);
}

inline void d_m_y(char *out, size_t n, int d, const char *mon, int y) {
  char db[8], yb[8], dl[16], yl[16];
  snprintf(db, sizeof(db), "%d", d);
  snprintf(yb, sizeof(yb), "%d", y);
  L().num(dl, sizeof(dl), db);
  L().num(yl, sizeof(yl), yb);
  if (L().rtl)
    snprintf(out, n, "%s %s %s", yl, mon, dl);
  else
    snprintf(out, n, "%s %s %s", dl, mon, yl);
}

inline void date_long(char *out, size_t n, int wday, int d, int mon, int year) {
  char db[8], yb[8], dl[16], yl[16];
  snprintf(db, sizeof(db), "%02d", d);
  snprintf(yb, sizeof(yb), "%d", year);
  L().num(dl, sizeof(dl), db);
  L().num(yl, sizeof(yl), yb);
  if (L().rtl)
    snprintf(out, n, "%s %s %s %s", yl, L().months[mon], dl, L().weekdays[wday]);
  else
    snprintf(out, n, "%s %s %s %s", L().weekdays[wday], dl, L().months[mon], yl);
}

inline void date_short(char *out, size_t n, int wday, int d, int mon) {
  char db[8], dl[16];
  snprintf(db, sizeof(db), "%02d", d);
  L().num(dl, sizeof(dl), db);
  if (L().rtl)
    snprintf(out, n, "%s %s %s", L().months[mon], dl, L().weekdays[wday]);
  else
    snprintf(out, n, "%s %s %s", L().weekdays[wday], dl, L().months[mon]);
}

// Display name of a zone: a label the user chose explicitly always wins
// (per-language when given); only derived labels (bare IANA specs) are
// replaced by the language's translated city, when it knows the name.
static_assert(LANG_COUNT == reterminal::ZONE_LANGS,
              "Zone label slots must match the language registry");

inline const char *zone_name(const reterminal::Zone &z) {
  if (z.custom[lang_idx])  // rules 1+2: a chosen label, per-lang or plain
    return z.labels[lang_idx];
  if (L().city != nullptr && z.iana[0] != 0) {  // rule 3: city mapping
    const char *n = L().city(z.iana);
    if (n != nullptr)
      return n;
  }
  return z.labels[lang_idx];  // rule 4: the derived tz label
}

inline const char *jalali_month(int m) { return L().jmonths[m]; }
inline const char *hijri_month(int m) { return L().hmonths[m]; }

}  // namespace lang
