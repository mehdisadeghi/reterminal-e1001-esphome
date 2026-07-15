# TODO

## Done

- [x] Multi-page dashboard with left/right button navigation
- [x] Deep sleep with all three buttons as ext1 wake pins; wake cause routed
      to page-flip / fetch actions
- [x] World clock (digital): five zones, dates, offsets vs local zone,
      local row inverted
- [x] Analog clocks page: 3+2 dials, thick hands, long 12/3/6/9 markers,
      offsets in labels, local dial inverted
- [x] Explicit timezone/DST computation (newlib TZ proved unreliable on ESP32)
- [x] Numbers page: Indoor | Outdoor | Device columns, big `°C`/`%` values
- [x] Onboard SHT4x shown as Device column
- [x] 24 h temperature and humidity graph pages (in solid / out dashed,
      auto-scaled grid, `data HH:MM` stamp)
- [x] Combined 7-day page: stacked temp+hum, vertical inverted TEMP/HUM
      titles, single centered legend, shared weekday axis
- [x] History fetch from HA REST API (streaming parser, day + week windows,
      independent all-or-nothing commits, 3 h cadence + green-button force)
- [x] `deep_sleep.prevent/allow` around fetch + redraw (week fetch was being
      truncated by run_duration)
- [x] Explicit time request on API connect + PCF8563 write-back (device
      never lived long enough for the 15 min poll)
- [x] Graph/page state in RTC slow memory (survives deep sleep)
- [x] Inverted status bar on every page: Jalali left, Gregorian center,
      battery right
- [x] `jalali.h`: header-only Gregorian → Solar Hijri (jdf algorithm)
- [x] Noqte test page: «نقطه» via Arabic presentation forms, no shaper
- [x] Page Auto-Cycle switch (HA entity, default on, one page per timer wake)
- [x] Emulated presses from HA: Next/Previous Page and Fetch History buttons
- [x] Secrets for Wi-Fi password, HA IP, API token; firewall pinhole
      documented (IoT → HA:8123 only)

## Open

- [ ] Inside/outside temps show identical values — mapping verified correct
      in firmware; needs `Got state` log lines from one wake to decide
      device vs HA side (duplicate/stale Zigbee entity suspected)
- [ ] Eyeball all page layouts on the real panel and nudge coordinates
      (tuned blind so far)
- [ ] Persian rendering of the status-bar date: small reshaper +
      presentation-form glyph set (Noqte or Vazirmatn); HarfBuzz rejected
      as overkill
- [ ] Decide fate of single 24 h pages once the combined page is approved
      (user note: "if turns out good, we remove the other two pages")
- [ ] Remove/replace the Noqte test page when done evaluating
- [ ] Fallback AP password is still `ChangeMe123`; strengthen or drop
      `ap:`/`captive_portal:`
- [ ] Optional: retained remote navigation (HA `select` read on wake) so
      emulated buttons work while the device sleeps
- [ ] Check/insert the CR1220 RTC backup cell (RTC read `STOP:ON` before
      first sync)
- [ ] Battery calibration curve is generic; validate against real
      discharge behavior
