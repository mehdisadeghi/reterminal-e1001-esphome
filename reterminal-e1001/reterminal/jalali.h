#pragma once

// Gregorian -> Solar Hijri (Jalali) conversion, header-only.
// Integer algorithm from the widely used jdf implementation.

namespace jalali {

static const char *const MONTHS[12] = {
    "Farvardin", "Ordibehesht", "Khordad", "Tir",    "Mordad", "Shahrivar",
    "Mehr",      "Aban",        "Azar",    "Dey",    "Bahman", "Esfand",
};

inline void from_gregorian(int gy, int gm, int gd, int &jy, int &jm, int &jd) {
  static const int G_D_M[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  int gy2 = (gm > 2) ? gy + 1 : gy;
  long days = 355666L + 365L * gy + (gy2 + 3) / 4 - (gy2 + 99) / 100 + (gy2 + 399) / 400 + gd +
              G_D_M[gm - 1];
  jy = -1595 + 33 * (int) (days / 12053L);
  days %= 12053L;
  jy += 4 * (int) (days / 1461L);
  days %= 1461L;
  if (days > 365) {
    jy += (int) ((days - 1) / 365);
    days = (days - 1) % 365;
  }
  if (days < 186) {
    jm = 1 + (int) (days / 31);
    jd = 1 + (int) (days % 31);
  } else {
    jm = 7 + (int) ((days - 186) / 30);
    jd = 1 + (int) ((days - 186) % 30);
  }
}

}  // namespace jalali
