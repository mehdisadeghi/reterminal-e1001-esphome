# reTerminal E1001 — ePaper Dashboard

ESPHome firmware for a Seeed reTerminal E1001 (ESP32-S3, 7.5" 800×480 B/W
ePaper): a seven-page, fully HA-configurable dashboard — world clocks
(digital and analog), indoor/outdoor/device climate, 24-hour and 7-day
history graphs, and a Persian type test page. Battery powered, deep sleep
5 min, awake `Wake Time` seconds after the last activity.

## Stack

**ESPHome** generates C++ from the YAML, built with PlatformIO for the
ESP32-S3 (Arduino framework). All custom logic lives in two headers
injected via `includes:` and called from YAML lambdas. The Arduino libs
used by the fetch path (`WiFi`, `WiFiClientSecure`, `HTTPClient`) are
declared explicitly (ESPHome builds with the library finder off). Fonts
are rasterized at compile time — each TTF is subset to the exact `glyphs:`
list; that's how a size-260 «نقطه» costs ~7 KB and no runtime font engine
exists.

## Files

| File | Purpose |
|------|---------|
| `esphome-reterminal-e1001.yaml` | Main config: substitutions, packages, boot orchestration |
| `reterminal-e1001/packages/*.yaml` | Config split by concern: hardware, network, controls, app |
| `reterminal-e1001/reterminal.h` | Umbrella include used by the lambdas |
| `reterminal-e1001/reterminal/pure.h` | Host-testable logic: parsers, calendars, timezone math |
| `reterminal-e1001/reterminal/device.h` | RTC state, sleep, REST access, snapshots, SD stack |
| `reterminal-e1001/reterminal/draw.h` | Page renderers |
| `reterminal-e1001/reterminal/jalali.h` | Gregorian → Solar Hijri conversion (jdf algorithm) |
| `reterminal-e1001/reterminal/tzdata.h` | Embedded IANA → POSIX-rule table (generated) |
| `tests/host_test.cpp` | Host-side unit tests for `pure.h` (see header for the one-liner) |
| `reterminal-e1001/*.ttf`, `fa_*_glyphs.yaml` | Persian fonts + generated glyph lists |
| `gen_fa_assets.py` | Pre-shapes the Khayyam corpus into `reterminal/khayyam_fa.h` + `fa_*_glyphs.yaml` |
| `reterminal-e1001/partitions.csv` | 32 MB flash: 2×3 MB OTA apps + 24 MB `hist` partition |
| `tzdata.csv` | Same tz data as a file — the SD-card override source |
| `gen_tzdata.py` | Regenerates both from upstream tzdata (`uv run --with tzdata python3 gen_tzdata.py`) |
| `validate_config.py` | Host-side `config.json` validator — mirrors the firmware's acceptance rules, cross-checks IANA names against `tzdata.csv` |
| `set_datetime.py` | Stamps a fresh one-shot `set_time` into `config.json` for air-gapped clock setting |
| `ha-helpers.yaml` | HA package creating the three queued-control helpers (Press, Keep Awake, Config Queue) |
| `sd-config-design.md` | Air-gapped SD-card configuration: design + implementation notes |
| `config.json.example` | Template for the SD `config.json` |

Only the main YAML and the `reterminal-e1001/` directory go to the
ESPHome builder folder (`make deploy`); everything project-specific lives
in that subfolder so the builder sees exactly one device.

## Pages

1. **World clock (digital)** — up to 5 zones with date, offset relative to
   the home zone, and a sun/moon glyph per row for that zone's day/night;
   home row inverted full-width. No status bar.
2. **Numbers** — 1–3 configured climate columns (`21.4°C` / `48%`); layout
   adapts to the enabled count.
3. **Temperature — 24 h** — one line per enabled column (solid / dashed /
   sparse), legend with labels and current values on the title line.
4. **Humidity — 24 h** — same layout.
5. **Week** — temperature and humidity stacked, vertical inverted TEMP/HUM
   titles, one centered legend, shared weekday axis, `data HH:MM` stamp.
6. **Analog clocks** — up to 5 dials (layout adapts to count), thick hands,
   long quarter markers, offsets in captions, home caption inverted,
   **night zones rendered as inverted dials** (window configurable).
7. **Noqte** — «نقطه» via Arabic presentation forms, no shaping engine.
8. **Khayyam** — a random quatrain from the Rubaiyat (106 pre-shaped at
   build time by `gen_fa_assets.py`; Vazirmatn body, Noqte title), picked
   with the ESP32's hardware RNG and rotated every 6 hours.

Inverted status bar on pages 2–6: Solar Hijri date left, Gregorian
(`Wed 15 Jul 2026`) centered; right side, growing leftward: battery %,
SD-card icon (when a card is inserted), `RTC!` warning (RTC had no valid
time at boot — dead/missing CR1220), device `T 25° H 48` (toggleable).

## Physical buttons

| Button | Asleep | Awake |
|--------|--------|-------|
| Left white (GPIO5) | wake + previous page | previous page |
| Right white (GPIO4) | wake + next page | next page |
| Green (GPIO3) | wake + force fetch | force fetch |

Page flips redraw instantly from RTC-cached values. Green also re-enables
a disabled radio for one wake (the recovery path). Every press beeps.

## HA entities

**Controls:** Beep (one short beep), Fetch History, Page Next,
Page Previous, Onboard LED, Buzzer (raw, continuous).

**Configuration:** Start Page (after cold boot; invalid → first page),
Home Timezone (city name), Zone 1–5 (spec, see below) with Zone 1–5
Enabled toggles, Column 1–3 (climate sources, see below), HA URL and HA
Token (REST endpoint — seeded from secrets, editable without reflashing),
HA Sync Interval (minutes between API syncs, default 15), Night From/To
(analog night window), Show 1–8 (per-page visibility; navigation,
auto-cycle, and start page skip hidden pages), Page Auto-Cycle (one page
per timer wake, default off), Radio (persisted wifi kill), Status Bar
Climate, Status Bar Solar Hijri, Pause Deep Sleep (RAM-only — any reset
re-enables sleep).

**HA-queued controls** — helpers, not device entities. Commanding a native
entity is an *event* over the live API connection: with the device asleep
it fails immediately ("Authenticated connection not ready yet") and HA
does not retry. A helper is pure HA-side *state*; the device subscribes to
it and receives the current state at every sync — that persistence is the
"queue". Create all three at once by installing `ha-helpers.yaml` as an HA
package: it goes to `<ha config>/packages/reterminal.yaml` (NOT into the
ESPHome builder folder — the builder treats every YAML there as a device
config and errors). Note they can never appear on the device page or its
⋮ → Helpers filter — input helpers cannot be linked to a device in HA's
registry — so pin them to a dashboard card next to the reTerminal's
controls, and optionally assign them the device's Area:

- `input_button.reterminal_press` ("Press") — one beep on the next wake;
  multiple presses collapse. The "awake now" signal.
- `input_boolean.reterminal_keep_awake` — while on, the device stays
  awake (beeps once when it receives it). Turn off to resume sleeping.
- `input_text.reterminal_config` — semicolon-separated config changes,
  e.g. `zone3=Asia/Dubai;show7=off;night=19-7;col1=Device=dev`. Keys:
  `zone1..zone5` and `col1..col3` (spec or empty), `home`, `night=F-T`,
  `start=N`, `sync=MIN`, `bar=DIGITS`, `cycle|showN=on|off`. Applied at the next sync
  (instantly while awake) into the device's persisted config entities,
  then the field is cleared as the acknowledgement + one beep — empty
  field = consumed, text still present = not delivered yet. Clearing
  requires "allow the device to perform Home Assistant actions" in the
  ESPHome integration's device settings; without it the same set re-applies
  on every sync (harmless, but the ack never comes).

## Zones are data, not code

Accepted spec forms, one per zone slot:

- `Asia/Tehran` — IANA name, resolved against the embedded table
  (~600 zones, *current* rules only — no historical transitions; regenerate
  with `gen_tzdata.py` after a tzdata release and reflash);
- `Memphis=America/Chicago` — IANA name with a custom display label;
- `City|std_min` — fixed offset in minutes east of UTC;
- `City|std_min|dst_min|m.w/h|m.w/h` — manual DST rules, `3.5/2` = March,
  5th (= last) Sunday, 02:00 local.

Invalid specs are skipped (logged); labels are limited to the compiled
glyph set (ASCII). Both clock pages render whatever the list holds — row
spacing and dial layout are computed from the count. The POSIX-rule parser
handles day-of-week rules, southern-hemisphere (wrapped) DST periods, and
Ireland-style negative DST. The same store is the target of the future SD
`config.json`; a `tzdata.csv` on the card is planned to override the
embedded table (see `sd-config-design.md`).

## Climate columns are data, not code

Column spec, one text entity per slot: `Label=dev` (onboard SHT4x, sampled
once per wake — no network) or `Label=<temp_entity>,<hum_entity>` (polled
from HA over REST). Empty = disabled. Whether a page needs the network is
*derived* from its sources: an all-`dev` configuration never touches HA —
no mode switch required. Entity IDs, HA URL, and token are all runtime
config (and SD fields), so sensor changes never require a reflash.

## Data flows & wake economics

- **HA → device** (native API, port 6053, Noise-encrypted, HA initiates):
  time and the queued helpers. This "HA sync" runs only on cold boot,
  green press, or every **HA Sync Interval** minutes — not per wake.
- **Device → HA** (REST, Bearer token, firewall pinhole IoT→HA:8123):
  current values via `/api/states`, 24 h and 7-day history via
  `/api/history/period` with explicit `end_time`, streamed through a
  `}`-split scanner (no big buffers), bucketed (7.5 min / 52.5 min),
  forward-filled, committed per row.
- **Gating:** a wake only waits for the network (10 s wifi, 2 s API — LAN)
  when a sync is due/forced or the shown page has HA-sourced columns with
  work to do (stale history, live values). Everything else is a
  radio-quiet redraw. Both history windows roll forward every wake, so
  dev-sourced curves stay continuous regardless of network.
- **History persistence:** series are int16 (value × 100) in RTC memory
  (survives deep sleep) and snapshotted hourly to the 24 MB `hist` flash
  partition (survives power loss and reflash; restored on cold boot; wear
  is negligible on a slot ring that size).

## Sleep model

No `run_duration`, no idle window. Every command — buttons (physical or
HA), LED, buzzer, any config change — bumps a deadline a 5 s grace ahead;
a 1 s watcher enters deep sleep (5 min) once the deadline passes, no
command script is running, and the queued keep-awake is off. A typical
timer wake is therefore ~12 s total. OTA freezes sleep entirely. State
that must survive sleep (page, graphs, cached values, fetch/sync stamps)
lives in RTC slow memory (~5 KB of 8 KB); preferences live in NVS flash
and survive power loss; graphs additionally survive power loss via the
hourly flash snapshots.

## Algorithms

- Howard Hinnant days-from-civil for all calendar math (ISO parsing,
  weekday, DST transitions).
- Explicit per-zone DST evaluation ("week'th Sunday of month at local
  hour") — newlib's TZ handling is unreliable on the ESP32.
- jdf integer algorithm for Gregorian → Solar Hijri.
- Nice-step graph auto-scaling; dashed lines by segment skipping; stroke
  thickness via perpendicular-offset line fans (`thick_line`).
- Arabic presentation forms (U+FB50–FEFF) printed in visual order for
  shaped Persian without HarfBuzz.

## Hardware used

SPI ePaper (48 KB framebuffer, ~5 s blocking full refresh, busy pin);
deep sleep with RTC-timer + ext1 wake on GPIO3/4/5 (wake pin identified
via `esp_sleep_get_ext1_wakeup_status`); 8 KB RTC slow memory; PCF8563
external RTC (I²C, CR1220 backup — the `VL`/`STOP` flags surface as the
`RTC!` warning); SHT4x temp/humidity; ADC battery sense behind a 1:2
divider with enable rail; LEDC buzzer PWM; SD slot (card-detect GPIO15,
power rail GPIO16 — filesystem access pending an external component);
CH340K UART0 for logs/flashing. GPIO3/45 strapping-pin and GPIO19/20
USB-Serial-JTAG validation warnings are inherent to the board wiring and
harmless.

## Secrets (`secrets.yaml` next to the config)

```yaml
wifi_password: "..."
ota_password: "..."
api_encryption_key: "..."      # openssl rand -base64 32
ha_http_ip: "192.168.x.x"      # HA LAN IP (firewall pinhole target)
ha_api_token: "..."            # long-lived token of a dedicated NON-ADMIN HA user
```

## SD card (air-gapped configuration)

FAT32 card, processed at boot and on insertion (two short beeps = applied,
one long = rejected; details and schema in `sd-config-design.md`):

- `/config.json` — full configuration as data (zones, columns, HA
  endpoint, pages, night window, one-shot `set_time` for clock setting
  with no network). Applied once per content change, backed up on the card
  as `/<date>.config.json` (newest 5 kept).
- `/tzdata.csv` — overrides the embedded IANA table (air-gapped tzdata
  updates without reflashing; regenerate with `gen_tzdata.py`).

## Operational notes

- **The first install of a partition-table change (this build) must go
  over USB** — OTA cannot rewrite the partition table. Subsequent updates
  OTA as usual.
- OTA only while awake: press a button first (or Pause Deep Sleep /
  keep-awake helper).
- Reflash/power loss clears RTC memory; graphs repopulate on the next
  fetch (or in device-only mode, refill at one sample per wake).
- The 7-day view needs HA recorder retention ≥ 7 days (default 10).
- The device must be added to HA's ESPHome integration manually by IP —
  discovery rarely catches a sleeping device.
- Radio off makes the device unreachable from HA; green button recovers.
