# TODO

## Done

- [x] Persian stage 1: Khayyam page (106 quatrains pre-shaped at build time
      by gen_fa_assets.py, Vazirmatn body + Noqte title, hardware-RNG pick
      rotated 6-hourly); Status Bar Pages config; Show 8 switch
- [x] Restructure: reterminal/ modules (pure/device/draw), YAML packages,
      host-side unit tests (calendar, jalali, zone/DST/column parsers)

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
- [x] Persian stage 2: runtime language select (en/fa) with Pack registry
      + per-language font sets; on-device reshaper (contextual forms,
      lam-alef, ZWNJ) for user-entered labels; CLDR city names; localized
      status bar (dates and climate readout); per-language zone labels
      with precedence rules; RTL-mirrored analog layout
- [x] Wi-Fi provisioning via captive portal + improv (no compiled
      credentials; `ap_password` secret for the fallback hotspot);
      three-strike backoff on failed wakes
- [x] Photo dial faces: baked-in `<city>.png` set (`$(IMAGES)`) with SD
      per-file override, runtime dither at the layout's radius, contrast
      halos, night negatives; Dial Photos / Night Mode toggles
- [x] PSRAM enabled (8 MB octal, S3R8)
- [x] Firmware version stamping (`make gen-version` → `git describe` in
      HA and on the debug page)
- [x] Debug page (double green press); long green press = remote config
- [x] Remote config from Config URL (daily or on demand, no SD needed)
- [x] Night Refresh Interval (slower cadence inside the night window)

## Open

- [ ] Rollback drill: flash a deliberately crashing build and watch the
      three-attempt auto-rollback restore the previous slot (the version
      stamp on the debug page verifies it)
- [ ] Measure real battery discharge (HA history slope) and validate the
      generic calibration curve; revisit cadence defaults with data
- [ ] Inside/outside temps reported identical — firmware mapping verified;
      needs `Got state` log lines from one wake (duplicate/stale Zigbee
      entity suspected)
- [ ] Eyeball all layouts on the real panel (tuned blind), incl. reduced
      zone counts and device-only pages
- [ ] Decide fate of the single 24 h pages (Show switches can hide them
      meanwhile); remove the noqte test page when done evaluating
- [ ] Check/insert the CR1220 RTC backup cell (RTC! warning / STOP flag)
- [ ] Battery end-of-life screen: below the lowest threshold the final
      update paints a large empty-battery glyph plus "Charge!" (localized,
      fa too), so a dead device shows why it stopped. Before that, thicken
      the battery column at successive thresholds so the drain is visible
- [ ] Accessibility: Large Status Bar option (twice the height, fonts
      scaled to match). Requires a real layout grid — status bar row plus
      content row, pages laid out inside the content row — so toggling the
      option pushes page content down instead of overlapping the bar
- [ ] Optional: sleep-proof remote navigation via a retained HA select
- [ ] Optional: remote firmware update over HTTPS (esp_https_ota into the
      passive slot, guarded by the existing rollback counter)
