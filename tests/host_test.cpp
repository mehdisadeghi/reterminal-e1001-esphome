// Host-side unit tests for the pure logic (no device needed):
//   c++ -std=c++17 -DRETERMINAL_HOST_TEST -I reterminal tests/host_test.cpp -o /tmp/rt_test && /tmp/rt_test

#include <cstdio>
#include <cstring>

#include "pure.h"
#include "hijri.h"
#include "jalali.h"
#include "lang.h"

static int fails = 0;
#define CHECK(cond)                                        \
  do {                                                     \
    if (!(cond)) {                                         \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      fails++;                                             \
    }                                                      \
  } while (0)

using namespace reterminal;

static void test_calendar() {
  CHECK(parse_iso_utc("2026-07-15T12:00:00") == 1784116800);
  CHECK(parse_iso_utc("1970-01-01T00:00:00") == 0);
  CHECK(parse_iso_utc("garbage") == 0);
  // 1970-01-01 was a Thursday; (days + 4) % 7 with 0 = Sunday
  CHECK((days_from_civil(2026, 7, 15) + 4) % 7 == 3);  // Wednesday
  CHECK((days_from_civil(2026, 7, 19) + 4) % 7 == 0);  // Sunday
}

static void test_jalali() {
  int jy, jm, jd;
  jalali::from_gregorian(2026, 7, 15, jy, jm, jd);
  CHECK(jy == 1405 && jm == 4 && jd == 24);  // 24 Tir 1405
  jalali::from_gregorian(2026, 3, 21, jy, jm, jd);
  CHECK(jy == 1405 && jm == 1 && jd == 1);  // Nowruz
}

static void test_lang() {
  char b[64];
  lang::lang_idx = 0;
  lang::date_long(b, sizeof(b), 3, 22, 6, 2026);
  CHECK(strcmp(b, "Wed 22 Jul 2026") == 0);
  lang::date_short(b, sizeof(b), 0, 5, 0);
  CHECK(strcmp(b, "Sun 05 Jan") == 0);
  lang::percent(b, sizeof(b), "34");
  CHECK(strcmp(b, "34%") == 0);
  lang::d_m_y(b, sizeof(b), 1, lang::jalali_month(4), 1405);
  CHECK(strcmp(b, "1 Mordad 1405") == 0);
  CHECK(lang::lang_index("fa") == 1 && lang::lang_index("nope") == 0);

  lang::lang_idx = 1;
  CHECK(lang::rtl());
  lang::num(b, sizeof(b), "23.4");
  CHECK(strcmp(b, "۲۳٫۴") == 0);  // persian digits + momayyez
  lang::percent(b, sizeof(b), "34");
  CHECK(strcmp(b, "٪۳۴") == 0);  // sign precedes digits in visual order
  lang::date_long(b, sizeof(b), 3, 22, 6, 2026);
  CHECK(strstr(b, "۲۰۲۶") == b);  // visual order starts with the year
  // presentation forms are 3-byte UTF-8: this line is ~55 bytes, which is
  // why the draw buffers are 64/96 (a 48-byte buffer cut چهارشنبه short)
  CHECK(strlen(b) > 47);
  lang::lang_idx = 0;
}

static void test_zone_names() {
  Zone z;
  parse_zone("Europe/Berlin", z);
  CHECK(!z.custom[0] && !z.custom[1]);  // derived label: mapping may apply
  lang::lang_idx = 1;
  CHECK(strcmp(lang::zone_name(z), z.labels[1]) != 0);  // fa shows the CLDR city
  parse_zone("Potsdam=Europe/Berlin", z);
  CHECK(z.custom[0] && z.custom[1]);  // plain label: every language
  CHECK(strcmp(lang::zone_name(z), "Potsdam") == 0);
  parse_zone("City|60", z);
  CHECK(strcmp(lang::zone_name(z), "City") == 0);
  lang::lang_idx = 0;
  parse_zone("Europe/Berlin", z);
  CHECK(strcmp(lang::zone_name(z), "Berlin") == 0);  // en: derived label
}

static void test_shaper() {
  // the runtime shaper must agree with the build-time pipeline: shape raw
  // Persian and compare against generated (pre-shaped) constants
  char out[40];
  shape_arabic("تهران", out, sizeof(out));
  CHECK(strcmp(out, lang::fa_city("Asia/Tehran")) == 0);
  shape_arabic("سه‌شنبه", out, sizeof(out));  // ZWNJ handling
  CHECK(strcmp(out, lang::fa::WEEKDAYS[2]) == 0);
  shape_arabic("شوال", out, sizeof(out));  // lam-alef inside a word
  CHECK(strcmp(out, lang::fa::HMONTHS[9]) == 0);
  shape_arabic("Plain", out, sizeof(out));  // no Arabic: untouched
  CHECK(strcmp(out, "Plain") == 0);

  // per-language labels, shaped at parse time
  Zone z;
  CHECK(parse_zone("en:Potsdam,fa:تهران=Europe/Berlin", z));
  CHECK(strcmp(z.labels[0], "Potsdam") == 0);
  CHECK(strcmp(z.labels[1], lang::fa_city("Asia/Tehran")) == 0);
  lang::lang_idx = 1;
  CHECK(strcmp(lang::zone_name(z), z.labels[1]) == 0);
  lang::lang_idx = 0;
  // a prefixed label binds only to its language: en falls through to the
  // derived tz label (there is no en city mapping)
  CHECK(parse_zone("fa:دفتر=Europe/Berlin", z));
  CHECK(z.custom[1] && !z.custom[0]);
  lang::lang_idx = 0;
  CHECK(strcmp(lang::zone_name(z), "Berlin") == 0);
  lang::lang_idx = 1;
  CHECK(strcmp(lang::zone_name(z), z.labels[1]) == 0);  // the shaped دفتر
  lang::lang_idx = 0;

  // multi-word Persian label (52-byte spec: needs max_length 96 entities)
  CHECK(parse_zone("en:Memphis,fa:خانه و باغچه=America/Chicago", z));
  CHECK(strcmp(z.labels[0], "Memphis") == 0);
  CHECK(z.labels[1][0] != 0 && strcmp(z.labels[1], "Memphis") != 0);
  CHECK(z.std_min == -360);

  // manual specs have no fallback: single prefixed label serves all slots
  CHECK(parse_zone("fa:کلبه|210", z));
  CHECK(strcmp(z.labels[0], z.labels[1]) == 0);
}

static void test_home_zone() {
  rebuild_zones("Asia/Tehran", "Europe/Berlin", "", "", "", "");
  CHECK(zone_count == 2 && home_zone == -1);  // empty = no home zone
  rebuild_zones("Asia/Tehran", "Potsdam=Europe/Berlin", "", "", "", "europe/berlin");
  CHECK(home_zone == 1);  // matched by IANA name, not by label
  rebuild_zones("Asia/Tehran", "Europe/Berlin", "", "", "", "Berlin");
  CHECK(home_zone == -1);  // labels are not tz names
  rebuild_zones("City|60", "Europe/Berlin", "", "", "", "City");
  CHECK(home_zone == -1);  // manual specs carry no IANA name
}

static void test_hijri() {
  int hy, hm, hd;
  hijri::from_gregorian(622, 7, 19, hy, hm, hd);
  CHECK(hy == 1 && hm == 1 && hd == 1);  // civil epoch (proleptic Gregorian)
  hijri::from_gregorian(2000, 1, 1, hy, hm, hd);
  CHECK(hy == 1420 && hm == 9 && hd == 24);  // 24 Ramadan 1420 (tabular)
  hijri::from_gregorian(2026, 7, 22, hy, hm, hd);
  CHECK(hy == 1448 && hm == 2 && hd == 6);  // 6 Safar 1448 (tabular)
}

static void test_zone_specs() {
  Zone z;
  CHECK(parse_zone("Berlin|60|120|3.5/2|10.5/3", z));
  CHECK(z.std_min == 60 && z.dst_min == 120 && z.start_week == 5);
  CHECK(parse_zone("Tehran|210", z));
  CHECK(z.std_min == 210 && z.dst_min == 210);
  CHECK(!parse_zone("NoOffset", z));
  CHECK(!parse_zone("Bad|9999", z));
  CHECK(!parse_zone("X|60|120|13.5/2|10.5/3", z));  // month 13

  // IANA forms against the embedded table
  CHECK(parse_zone("Europe/Berlin", z));
  CHECK(strcmp(z.labels[0], "Berlin") == 0 && z.std_min == 60 && z.dst_min == 120);
  CHECK(parse_zone("Memphis=America/Chicago", z));
  CHECK(strcmp(z.labels[0], "Memphis") == 0 && z.std_min == -360 && z.dst_min == -300);
  CHECK(parse_zone("America/Mexico_City", z));
  CHECK(strcmp(z.labels[0], "Mexico City") == 0 && z.dst_min == z.std_min);
  CHECK(parse_zone("Asia/Tehran", z));
  CHECK(z.std_min == 210);
  CHECK(!parse_zone("No/Such_Zone", z));
}

static void test_dst_offsets() {
  time_t jul = parse_iso_utc("2026-07-15T12:00:00");
  time_t jan = parse_iso_utc("2026-01-15T12:00:00");
  Zone z;

  parse_zone("Europe/Berlin", z);
  CHECK(zone_offset_min(z, jul) == 120);
  CHECK(zone_offset_min(z, jan) == 60);

  parse_zone("Australia/Sydney", z);  // southern hemisphere: DST wraps the year
  CHECK(zone_offset_min(z, jul) == 600);
  CHECK(zone_offset_min(z, jan) == 660);

  parse_zone("Europe/Dublin", z);  // negative-DST formulation
  CHECK(zone_offset_min(z, jul) == 60);
  CHECK(zone_offset_min(z, jan) == 0);

  parse_zone("Asia/Tehran", z);
  CHECK(zone_offset_min(z, jul) == 210 && zone_offset_min(z, jan) == 210);
}

static void test_columns() {
  Column c{};
  CHECK(parse_column("Device=dev", c));
  CHECK(c.is_dev && strcmp(c.label, "Device") == 0);
  CHECK(parse_column("Apt=sensor.a_temperature,sensor.a_humidity", c));
  CHECK(!c.is_dev && strcmp(c.temp_entity, "sensor.a_temperature") == 0 &&
        strcmp(c.hum_entity, "sensor.a_humidity") == 0);
  CHECK(!parse_column("NoEquals", c));
  CHECK(!parse_column("X=sensor.only_one", c));
  CHECK(!parse_column("=dev", c));
}

static void test_night() {
  night_from = 18;
  night_to = 6;
  CHECK(is_night(23) && is_night(3) && !is_night(12));
  night_from = 6;
  night_to = 18;  // inverted window
  CHECK(is_night(12) && !is_night(23));
  night_from = 18;
  night_to = 6;
}

int main() {
  test_calendar();
  test_jalali();
  test_hijri();
  test_lang();
  test_zone_names();
  test_shaper();
  test_home_zone();
  test_zone_specs();
  test_dst_offsets();
  test_columns();
  test_night();
  if (fails == 0)
    printf("all tests passed\n");
  return fails == 0 ? 0 : 1;
}
