# TODO

## Done

- [x] Multi-page dashboard with button navigation; all three buttons wake
      from deep sleep, wake pin routed to flip/fetch actions
- [x] World clock (digital): runtime zone list (1–5), dates, offsets vs
      selectable home zone, home row inverted
- [x] Analog clocks: layout adapts to zone count, thick hands/markers,
      offsets in captions, home caption inverted, night zones as inverted
      dials with configurable window
- [x] Zones as data: compact spec text entities + per-zone Enabled
      toggles + Home Timezone by name; parser/validator in firmware
- [x] IANA zone names ("Asia/Tehran", "Memphis=America/Chicago"): embedded
      name → current-POSIX-rule table (~16 KB flash, tzdata 2026c) with a
      full POSIX rule parser (any weekday, wrapped/negative DST); caught a
      real change — BC goes permanent UTC-7 in Nov 2026
- [x] Explicit per-zone DST computation (newlib TZ unreliable on ESP32)
- [x] Numbers page: Device | Indoor | Outdoor; single Device column in
      device-only mode
- [x] 24 h graph pages with inline legend; 7-day combined page with
      vertical inverted titles, shared weekday axis, data stamp
- [x] History fetch over REST: streaming parser, explicit end_time,
      day/week windows committing independently, 3 h cadence, green force
- [x] Wake economics: network waits and fetches gated by the shown page;
      empty-data page flips trigger an immediate fetch; short LAN waits
- [x] Climate columns as data: 1-3 columns, each `dev` or HA entity pair
      polled over REST; network needs derived from sources (Device Only
      mode removed); HA URL/token/entities all runtime config
- [x] HA Sync Interval: API sync (time + queued helpers) on boot, green,
      or every N minutes instead of per wake; fixed 2 s post-work grace
- [x] 32 MB flash: custom partition table (2×3 MB OTA apps) + 24 MB hist
      partition; hourly history snapshots survive power loss and reflash
- [x] Sleep model: activity deadline (HA-configurable Wake Time), every
      command bumps it, watcher sleeps only when idle and no script runs;
      OTA guard; Pause Deep Sleep (RAM-only) for tinkering
- [x] Radio kill switch (persisted); green button recovery for one wake
- [x] Per-page Show 1–7 switches; navigation/auto-cycle/start page skip
      hidden pages
- [x] Start Page (cold boot), Page Auto-Cycle (default off)
- [x] Status bar on pages 2–6: Jalali left, Gregorian+year center;
      battery, SD-card icon, RTC! warning, optional device climate right
- [x] Queued "Press" via HA Button helper (timestamp state, collapses
      multiple presses, one beep on next wake); instant Beep button
- [x] Emulated Page Next/Previous/Fetch History buttons in HA
- [x] jalali.h (Gregorian → Solar Hijri); noqte test page via Arabic
      presentation forms
- [x] SD-card configuration implemented (Arduino SD lib over the shared
      SPI bus): /config.json validate-then-apply through the shared
      change-set path, content-CRC idempotence, dated backups (keep 5),
      one-shot set_time (writes the RTC), two-beep ack / long-beep reject,
      processed at boot and on card insertion
- [x] /tzdata.csv on the SD card overrides the embedded IANA table
- [x] Host tools: gen_tzdata.py (tz table), validate_config.py (mirrors
      firmware acceptance rules), set_datetime.py (one-shot set_time
      stamping); workflow documented in sd-config-design.md
- [x] Day/night sun/moon glyphs on the digital world clock rows

## Open

- [ ] Persian support, stage 1: build-time pipeline pre-shaping the Khayyam
      corpus (../righter khayyam_fa.yaml, 28 KB) into presentation-form
      strings + exact glyph sets; new Khayyam page (no status bar) showing a
      random quatrain (esp_random), rotated every few hours; Vazirmatn body
      + Noqte title (verify Vazirmatn ships presentation-form cmap entries,
      else bake them with fontTools)
- [ ] Persian support, stage 2: on-device reshaper (contextual forms,
      lam-alef, ZWNJ) for arbitrary/SD-provided text and the Persian
      status-bar date
- [ ] Status bar visibility per page as config (currently hardcoded: off on
      world clock and noqte pages)
- [ ] Restructure for scale: move the C++ from the single header into a
      proper ESPHome external component (components/reterminal/), split the
      YAML with packages/!include, host-side unit tests for pure logic
      (tz/POSIX parsers, jalali, reshaper, config validation), CI compile
- [ ] Commit the accumulated backlog (everything since 2cc1f1c)

- [ ] Inside/outside temps reported identical — firmware mapping verified;
      needs `Got state` log lines from one wake (duplicate/stale Zigbee
      entity suspected)
- [ ] Verify SD bus sharing on real hardware (Arduino SD lib and ESPHome's
      SPI driver share the bus serialized in the loop; also confirm the SD
      CS pin is GPIO14 per Seeed's schematic)
- [ ] Eyeball all layouts on the real panel (tuned blind), incl. reduced
      zone counts and device-only pages
- [ ] Persian status-bar date via reshaper + presentation-form glyphs
- [ ] Decide fate of the single 24 h pages (Show switches can hide them
      meanwhile); remove the noqte test page when done evaluating
- [ ] Fallback AP password is still `ChangeMe123`; strengthen or drop
      `ap:`/`captive_portal:`
- [ ] Check/insert the CR1220 RTC backup cell (RTC! warning / STOP flag)
- [ ] Battery calibration curve is generic; validate against real
      discharge behavior
- [ ] Optional: sleep-proof remote navigation via a retained HA select
