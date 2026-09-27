#pragma once

// Gregorian -> Lunar Hijri. Inside the covered window the official
// Iranian sighting-based calendar is used exactly, from the
// persian-calendar project's month-length table (hijri_ir.h — data, not
// a formula: sighting decisions cannot be computed). Outside it, the
// tabular (arithmetic) calendar: 30-year cycle, leap years
// {2,5,7,10,13,16,18,21,24,26,29}, astronomical epoch JDN 1948439 —
// within ±1 day of observation.

#include "hijri_ir.h"
#include "pure.h"

namespace hijri {

static const char *const MONTHS[12] = {
    "Muharram", "Safar",   "Rabi I",  "Rabi II", "Jumada I",  "Jumada II",
    "Rajab",    "Shaban",  "Ramadan", "Shawwal", "Dhul-Qada", "Dhul-Hijja",
};

inline bool leap_year(int year_in_cycle) {
  switch (year_in_cycle) {
    case 2:
    case 5:
    case 7:
    case 10:
    case 13:
    case 16:
    case 18:
    case 21:
    case 24:
    case 26:
    case 29:
      return true;
  }
  return false;
}

inline bool from_table(long jdn, int &hy, int &hm, int &hd) {
  long days = jdn - IR_START_JDN;
  if (days < 0)
    return false;
  long acc = 0;
  for (int m = 0; m < IR_YEARS * 12; m++) {
    int len = (IR_MONTHS[m / 12] >> (11 - m % 12)) & 1 ? 30 : 29;
    if (days < acc + len) {
      hy = IR_START_YEAR + m / 12;
      hm = m % 12 + 1;
      hd = (int) (days - acc) + 1;
      return true;
    }
    acc += len;
  }
  return false;
}

// Returns true when the exact Iranian table produced the date, false on
// the tabular fallback — the calendar page annotates which one it shows.
inline bool from_gregorian(int gy, int gm, int gd, int &hy, int &hm, int &hd) {
  // days_from_civil counts from 1970-01-01 (JDN 2440588)
  long jdn = reterminal::days_from_civil(gy, gm, gd) + 2440588;
  if (from_table(jdn, hy, hm, hd))
    return true;
  // tabular fallback, astronomical epoch JDN 1948439
  long k = jdn - 1948439;
  long cycle = k / 10631;  // days in 30 lunar years
  k %= 10631;
  int y = 1;
  while (k >= 354 + (leap_year(y) ? 1 : 0)) {
    k -= 354 + (leap_year(y) ? 1 : 0);
    y++;
  }
  hy = (int) (cycle * 30 + y);
  hm = 1;
  while (true) {
    int len = (hm % 2 == 1) ? 30 : 29;
    if (hm == 12 && leap_year(y))
      len = 30;
    if (k < len)
      break;
    k -= len;
    hm++;
  }
  hd = (int) k + 1;
  return false;
}

}  // namespace hijri
