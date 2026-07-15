// Host-side unit tests for the pure logic (no device needed):
//   c++ -std=c++17 -DRETERMINAL_HOST_TEST -I reterminal tests/host_test.cpp -o /tmp/rt_test && /tmp/rt_test

#include <cstdio>

#include "pure.h"
#include "jalali.h"

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
  CHECK(strcmp(z.city, "Berlin") == 0 && z.std_min == 60 && z.dst_min == 120);
  CHECK(parse_zone("Memphis=America/Chicago", z));
  CHECK(strcmp(z.city, "Memphis") == 0 && z.std_min == -360 && z.dst_min == -300);
  CHECK(parse_zone("America/Mexico_City", z));
  CHECK(strcmp(z.city, "Mexico City") == 0 && z.dst_min == z.std_min);
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
  test_zone_specs();
  test_dst_offsets();
  test_columns();
  test_night();
  if (fails == 0)
    printf("all tests passed\n");
  return fails == 0 ? 0 : 1;
}
