#pragma once

// Gregorian -> Lunar Hijri, header-only: the tabular (arithmetic) Islamic
// calendar — 30-year cycle, leap years {2,5,7,10,13,16,18,21,24,26,29},
// civil epoch JDN 1948440 (16 July 622 Julian). Within ±1 day of
// observation-based calendars, which is fine for a status bar date.

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

inline void from_gregorian(int gy, int gm, int gd, int &hy, int &hm, int &hd) {
  // days_from_civil counts from 1970-01-01 (JDN 2440588); the Islamic
  // civil epoch is JDN 1948440
  long k = reterminal::days_from_civil(gy, gm, gd) + 2440588 - 1948440;
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
}

}  // namespace hijri
