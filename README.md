# reTerminal E1001 — ePaper Dashboard

ESPHome firmware for a Seeed reTerminal E1001 (ESP32-S3, 7.5" 800×480 B/W
ePaper): an eight-page, fully HA-configurable dashboard — world clocks
(digital and analog), indoor/outdoor/device climate, 24-hour and
multi-day history graphs, and Persian typography pages. Battery powered:
deep sleep between refreshes (**Refresh Interval**, default 3 min), awake
a 2 s grace after autonomous work and 30 s after user interaction.

## Stack

**ESPHome** generates C++ from the YAML, built for the ESP32-S3 with the
Arduino framework — which since ESPHome 2026.7 compiles as an ESP-IDF
component with selective library compilation. All custom logic lives in
headers injected via `includes:` and called from YAML lambdas. The
Arduino bundled libs used by the fetch and SD paths are switched on by
listing their core-3.x names under `libraries:` (`HTTPClient`, `Network`,
`NetworkClientSecure`, `SPI`, `FS`, `SD` — unknown names fall through to
a registry lookup and fail the build). Fonts
are rasterized at compile time — each TTF is subset to the exact `glyphs:`
list; that's how a size-260 «نقطه» costs ~7 KB and no runtime font engine
exists.

## Files

| File | Purpose |
|------|---------|
| `esphome-reterminal-e1001.yaml` | Main config: identity, sources, packages; adopted units pull it from GitHub as a remote package |
| `components/reterminal/__init__.py` | ESPHome component: brings the C++ into the build, generates the dial photo header, stamps the version |
| `reterminal-e1001/packages/*.yaml` | Config split by concern: core (includes, boot), hardware, network, controls, app |
| `components/reterminal/src/reterminal.h` | Umbrella include used by the lambdas |
| `components/reterminal/src/reterminal/pure.h` | Host-testable logic: parsers, calendars, timezone math |
| `components/reterminal/src/reterminal/device.h` | RTC state, sleep, REST access, snapshots, SD stack |
| `components/reterminal/src/reterminal/draw.h` | Page renderers |
| `components/reterminal/src/reterminal/jalali.h` | Gregorian → Solar Hijri conversion (jdf algorithm) |
| `components/reterminal/src/reterminal/tzdata.h` | Embedded IANA → POSIX-rule table (generated) |
| `tests/host_test.cpp` | Host-side unit tests for `pure.h` (see header for the one-liner) |
| `reterminal-e1001/*.ttf`, `fa_*_glyphs.yaml` | Persian fonts + generated glyph lists |
| `tools/gen_fa_assets.py` | Pre-shapes the Khayyam corpus into `reterminal/khayyam_fa.h` + `fa_*_glyphs.yaml` |
| `components/reterminal/src/reterminal/dial_image.h` | Photo dial faces: embedded set + SD override, runtime scale + Floyd-Steinberg dither, per-(city, radius) cache |
| `components/reterminal/src/reterminal/pngle.[ch]` | Vendored streaming PNG decoder (kikuchan/pngle, MIT); inflate comes from the ESP32 ROM's miniz |
| `tzdata.csv` | Same tz data as a file — the SD-card override source |
| `tools/gen_tzdata.py` | Regenerates both from upstream tzdata (`uv run tools/gen_tzdata.py`) |
| `tools/validate_config.py` | Host-side `config.json` validator — mirrors the firmware's acceptance rules, cross-checks IANA names against `tzdata.csv` |
| `tools/set_datetime.py` | Stamps a fresh one-shot `set_time` into `config.json` for air-gapped clock setting |
| `ha-helpers.yaml` | Per-unit HA package template for the three queued-control helpers (Press, Keep Awake, Config Queue) |
| `secrets.yaml.example` | Template for the gitignored `secrets.yaml` the build needs |
| `commissioning.md` | Bringing a new unit into service: USB flash, Wi-Fi, HA, helpers, updates |
| `docs/adr/` | Architecture Decision Records — why the design choices were made, and what would make us revisit them |
| `sd-config-design.md` | Air-gapped SD-card configuration & firmware update: design + implementation notes |
| `config.json.example` | Template for the SD `config.json` |
| `Makefile` | `build` / `flash` / `helpers` / `logs` / `test` (see below) |

The repository holds only the generic firmware and examples. Everything
specific to one installation stays outside it: `secrets.yaml`, an
untracked `GNUmakefile` for the HA host and ssh user, the dial photos,
and each unit's runtime configuration, which lives on the unit and is
managed from HA.

## Build, flash (Makefile)

Units are built and updated by the ESPHome Builder (see
`commissioning.md`); the Makefile is for local work. Local builds take the
component and fonts from this tree instead of GitHub.

- `make build` — local compile (pinned `ESPHOME_VERSION`; needs
  `secrets.yaml` beside the yaml, since its values are baked in),
  producing the app image for the SD firmware update. `YAML=` points it
  at an untracked config that includes the main one and lists dial
  photos.
- `make flash PORT=/dev/cu.usbserial-…` — first install of a unit over
  USB (see `commissioning.md`).
- `make helpers UNIT=reterminal-e1001-a1b2c3` — install that unit's
  helper package on the HA host.
- `make logs UNIT=… | PORT=…` — one unit's log, over the API while it is
  awake or from USB.
- `make test` — host unit tests + `config.json.example` validation.

## Pages

1. **Analog clocks** — up to 5 dials (layout adapts to count), thick hands,
   long quarter markers, offsets in captions, home caption inverted,
   **night zones rendered as inverted dials** (window configurable). A
   zone with a photo (see *Photo dial faces*) gets it as the dial face —
   as a negative at night — with contrast halos under hands and ticks.
   RTL mirrors the whole layout (zone order and staircase alike).
2. **World clock (digital)** — up to 5 zones with date, offset relative to
   the home zone, and a sun/moon glyph per row for that zone's day/night;
   home row inverted full-width. No status bar.
3. **Numbers** — 1–3 configured climate columns (`21.4°C` / `48%`); layout
   adapts to the enabled count.
4. **Temperature — 24 h** — one line per enabled column (solid / dashed /
   sparse), legend with labels and current values on the title line.
5. **Humidity — 24 h** — same layout.
6. **Combo** — temperature and humidity stacked over **Combo Days**
   (default 7 = one week), vertical inverted TEMP/HUM titles, one legend,
   shared day axis (weekday names up to a week, day-of-month beyond),
   `data HH:MM` stamp.
7. **Noqte** — «نقطه» via Arabic presentation forms, no shaping engine.
8. **Khayyam** — a random quatrain from the Rubaiyat (106 pre-shaped at
   build time by `gen_fa_assets.py`; Vazirmatn body, Noqte title), picked
   with the ESP32's hardware RNG and rotated every 6 hours.

Inverted status bar on every page except those listed in **Status Bar
Excluded** (comma-separated, default `7,8`): Solar and/or Lunar Hijri
date left (toggleable), Gregorian (`Wed 15 Jul 2026`) centered; right
side, growing leftward: battery %, lit-bulb icon (device held awake),
SD-card icon (card inserted), `RTC!` warning (RTC had no valid time at
boot — dead/missing CR1220), device climate `T 25° H 48` (toggleable,
localized: Persian digits and دما/رطوبت initials under fa). An optional
1 px **Battery Bar** stands on the right edge (left in RTL), filled
bottom-up.

## Photo dial faces

A zone whose last IANA segment (lowercased) matches a `<city>.png` gets
that photo as its analog dial face — `Europe/Berlin` → `berlin.png`,
`America/Mexico_City` → `mexico_city.png`. Two optional sources, card on
top:

1. `/<city>.png` on the SD card — per-device override, decoded on the fly
   (pngle; non-interlaced PNGs of any common bit depth).
2. `dial_images` in the unit's config — city → URL or local path, e.g.
   `berlin: http://homeassistant.local:8123/local/dials/berlin.png`;
   fetched and baked into the firmware at build time, works with no
   card. The repository ships none.

Either way the photo is center-cropped square, brightness/contrast lifted
for 1-bit rendering, scaled to whatever radius the layout engine chose,
Floyd-Steinberg dithered, and cached per (city, diameter). Hands, ticks,
hub, and inside labels clear a 2 px opposite-color halo over photos so
they keep full contrast on any dither; at night the face renders as a
negative. **Dial Photos** switches the whole feature off.

## Physical buttons

| Button | Asleep | Awake |
|--------|--------|-------|
| Left white (GPIO5) | wake + previous page | previous page |
| Right white (GPIO4) | wake + next page | next page |
| Green (GPIO3) | wake + force fetch | short: force fetch · double: debug page · long (≥1 s): remote config fetch |

Page flips redraw instantly from RTC-cached values. Green also re-enables
a disabled radio for one wake (the recovery path). Every press beeps.
The **debug page** (double green press) overlays a tabular readout —
firmware version (git describe), ESPHome/IDF versions, chip, memory,
Wi-Fi, battery, RTC, uptime, intervals, zones, Config URL age — and any
page button returns to the page under it.

## HA entities

**Controls:** Beep (one short beep), Fetch History, Page Next,
Page Previous, Onboard LED, Buzzer (raw, continuous).

**Configuration:** Start Page (after cold boot; invalid → first page),
Home Zone (IANA name, e.g. `Europe/Berlin`: the first zone resolving to
it gets the inverted row/dial caption and becomes the offset base;
empty/unmatched = no home, offsets vs. UTC), Zone 1–5 (spec, see below)
with Zone 1–5 Enabled toggles, Column 1–3 (climate sources, see below),
HA URL and HA Token (REST endpoint — seeded from secrets, editable
without reflashing), Language (`en`/`fa` — direction, digits, dates, and
fonts follow), Display Refresh Interval (the deep-sleep wake interval
*and* the redraw interval while held awake, default 3 min), Night
Refresh Interval (used instead while the home zone is inside the night
window; 0 = off), HA Sync Interval (minutes between API syncs, default
15), Combo Days (span of the combined page, default 7; changing it
resets the window and refetches), Night From/To (night window), Night
Mode (dial inversion on/off), Dial Photos (photo faces on/off), Analog
Clock Offsets, Analog Labels Inside, Show 1–8 (per-page visibility;
navigation, auto-cycle, and start page skip hidden pages), Status Bar
Excluded (comma-separated pages, default `7,8`), Battery Bar, Status Bar
Climate / Solar Hijri / Lunar Hijri, Config URL (remote `config.json`,
see below; empty = off), Page Auto-Cycle (one page per timer wake,
default off), Radio (persisted wifi kill), Pause Sleep (while awake)
(RAM-only — any reset re-enables sleep).

**HA-queued controls** — helpers, not device entities. Commanding a native
entity is an *event* over the live API connection: with the device asleep
it fails immediately ("Authenticated connection not ready yet") and HA
does not retry. A helper is pure HA-side *state*; the device subscribes to
it and receives the current state at every sync — that persistence is the
"queue". The helpers carry the unit's name, which it takes from its MAC
(`reterminal-e1001-a1b2c3` → `reterminal_e1001_a1b2c3`), so every unit
runs the same config, and the ids are subscribed at runtime. `make helpers
UNIT=<name>` installs a unit's three from the `ha-helpers.yaml` template
as `<ha config>/packages/<unit>.yaml`. Note they can never appear on the device page or its
⋮ → Helpers filter — input helpers cannot be linked to a device in HA's
registry — so pin them to a dashboard card next to the reTerminal's
controls, and optionally assign them the device's Area:

- `input_button.<unit>_press` ("Press") — one beep on the next
  wake; multiple presses collapse. The "awake now" signal.
- `input_boolean.<unit>_keep_awake` — while on, the device
  stays awake (beeps once when it receives it). Turn off to resume
  sleeping.
- `input_text.<unit>_config` — semicolon-separated config
  changes, e.g. `zone3=Asia/Dubai;show7=off;night=19-7;col1=Device=dev`.
  Keys: `zone1..zone5` and `col1..col3` (spec or empty),
  `home=IANA_NAME`, `night=F-T`, `start=N`, `sync=MIN`, `refresh=MIN`,
  `nrefresh=MIN`, `days=N`, `barskip=7,8`, `lang=en|fa`,
  `cycle|showN=on|off`. Applied at the next sync (instantly while awake)
  into the device's persisted config entities, then the field is cleared
  as the acknowledgement + one beep — empty field = consumed, text still
  present = not delivered yet. Clearing requires "allow the device to
  perform Home Assistant actions" in the ESPHome integration's device
  settings; without it the same set re-applies on every sync (harmless,
  but the ack never comes).

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
Ireland-style negative DST. The SD `config.json` targets the same store,
and a `tzdata.csv` on the card overrides the embedded table (see
`sd-config-design.md`).

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
  current values via `/api/states`, 24 h and Combo-Days history via
  `/api/history/period` with explicit `end_time`, streamed through a
  `}`-split scanner (no big buffers), bucketed (window ÷ 192 points),
  forward-filled, committed per row.
- **Gating:** a wake only waits for the network (10 s wifi, 2 s API — LAN)
  when a sync is due/forced or the shown page has HA-sourced columns with
  work to do (stale history, live values). Flipping onto a data page whose
  HA values are missing or stale triggers a fetch, asleep or awake.
  Everything else is a radio-quiet redraw. Both history windows roll
  forward every wake, so dev-sourced curves stay continuous regardless of
  network.
- **History persistence:** series are int16 (value × 100) in RTC memory
  (survives deep sleep) and snapshotted hourly to the 24 MB `hist` flash
  partition (survives power loss and reflash; restored on cold boot; wear
  is negligible on a slot ring that size).

## Sleep model

No `run_duration`, no idle window. Autonomous work bumps a deadline a 2 s
grace ahead; user interaction — buttons (physical or HA), LED, buzzer,
any config change — bumps it 30 s ahead. A 1 s watcher enters deep sleep
for **Refresh Interval** minutes once the deadline passes, no command
script is running, and the queued keep-awake is off; while the device is
held awake, the same watcher redraws whenever a refresh interval elapses,
so the clocks never freeze. A typical timer wake is ~12 s total. OTA
freezes sleep entirely. State
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
  shaped Persian without HarfBuzz — pre-shaped at build time for UI
  strings and CLDR city names, and shaped on device (contextual joining,
  lam-alef, ZWNJ; table generated from arabic-reshaper's data) for
  user-entered zone labels, once per config change.
- **Analog layout engine** (`draw_analog_clocks`): dial-first. Counts map
  to row shapes (1–3 one row, 4 = 2+2, 5 = 2+3). The radius is solved
  from the real page — height above the status bar when that page shows
  it, bezel margins, the battery hairline, and the *measured* pixel
  height of every caption in the exact font that will render it. The
  caption face then follows the dial: the largest of 48/32/20 px whose
  resulting radius still clears that tier's threshold — never the
  reverse. Two-row layouts interleave on N evenly spaced columns and
  overlap vertically: the binding constraint is the circle distance
  √(d² + dy²) ≥ 2R + 12 between neighbouring columns, with the row
  offset dy solved jointly with R by scanning R downward; top-row
  captions render above their dials and bottom-row below, keeping text
  out of the interleave zone. Centers derive from the final radius with
  equal gaps, so no side collects leftover whitespace. "Analog Labels
  Inside" moves captions onto the faces and drops both caption bands
  from the vertical span — the largest dials of all. Under RTL every
  center mirrors (`x → 800 − x`), flipping zone order, the staircase
  (CECE/ECEC → ECEC/CECE), and the margins in one stroke.
- **Photo faces**: embedded images are pre-adjusted grayscale squares
  (build-time crop + brightness ×1.18 / contrast ×1.15); SD PNGs are
  streamed through pngle with inflate from the ESP32 ROM's miniz,
  carrying partial syntactic units across reads. Both paths share the
  same nearest-neighbor scale + serpentine-free Floyd-Steinberg dither
  to 1-bit at the dial's exact diameter, cached per (city, diameter)
  and re-dithered when the layout changes the radius. Contrast is
  guaranteed by construction: every stroke over a photo first clears a
  2 px halo in its opposite color.

## Hardware used

SPI ePaper (48 KB framebuffer, ~5 s blocking full refresh, busy pin);
8 MB octal PSRAM (ESP32-S3R8, enabled — allocations over 16 KB leave the
internal heap, notably the PNG decoder's ~44 KB state); deep sleep with
RTC-timer + ext1 wake on GPIO3/4/5 (wake pin identified
via `esp_sleep_get_ext1_wakeup_status`); 8 KB RTC slow memory; PCF8563
external RTC (I²C, CR1220 backup — the `VL`/`STOP` flags surface as the
`RTC!` warning); SHT4x temp/humidity; ADC battery sense behind a 1:2
divider with enable rail; LEDC buzzer PWM; SD slot (FAT32 via the Arduino
`SD` lib on the shared SPI bus — CS GPIO14, card-detect GPIO15, power
rail GPIO16); CH340K UART0 for logs/flashing. GPIO3/45 strapping-pin and
GPIO19/20
USB-Serial-JTAG validation warnings are inherent to the board wiring and
harmless.

## Secrets (`secrets.yaml` next to the config)

Copy `secrets.yaml.example`; every value is baked into the build:

```yaml
ota_password: "..."
api_encryption_key: "..."      # openssl rand -base64 32
ha_http_ip: "192.0.2.10"       # HA LAN IP (firewall pinhole target)
ha_api_token: "..."            # long-lived token of a dedicated NON-ADMIN HA user
```

The Builder compiles with the HA host's `/config/esphome/secrets.yaml`,
which needs the same keys, plus `wifi_ssid` and `wifi_password` for the
`wifi:` block adoption adds. Wi-Fi is otherwise provisioned at runtime
(captive portal or Improv); the fallback hotspot is open, since it only
appears while the unit has no working Wi-Fi.

## SD card (air-gapped configuration)

FAT32 card, processed at boot and on insertion (two short beeps = applied,
one long = rejected; details and schema in `sd-config-design.md`):

- `/config.json` — full configuration as data (zones, columns, HA
  endpoint, pages, intervals, night window, one-shot `set_time` for clock
  setting with no network). Applied once per content change, backed up on
  the card as `/<date>.config.json` (newest 5 kept).
- `/tzdata.csv` — overrides the embedded IANA table (air-gapped tzdata
  updates without reflashing; regenerate with `gen_tzdata.py`).
- `/<city>.png` — per-zone photo dial face, overriding the baked-in image
  for that city (see *Photo dial faces*); re-read on card insertion.
- `/firmware.bin` — air-gapped firmware update: the app image from
  `make build` is flashed into the passive OTA slot (fingerprint-gated,
  CRC-compared, validated before the boot slot switches) and booted. An
  image that never completes a boot rolls back to the previous slot after
  three attempts — the same guard arms on network OTA. Details in
  `sd-config-design.md`.

The same `config.json` can also be served over the network: set **Config
URL** and the device fetches it once per day on a wake with the network
up, or immediately on a long green press — same validation, CRC
idempotence, beeps, and all-or-nothing apply as the card, no card
involved. HTTPS is accepted without certificate pinning (the URL is the
trust anchor). Pre-flight either transport with
`uv run tools/validate_config.py yourfile.json`.

## Operational notes

- **The first install of a partition-table change (this build) must go
  over USB** — OTA cannot rewrite the partition table. Subsequent updates
  go through the ESPHome Builder or the SD card. The table holds two OTA
  app slots and the 24 MB `hist` partition; units first flashed before
  it was generated by ESPHome keep their older table (3 MB slots), which
  every build must still fit.
- Updates reach only awake units: switch on the unit's Keep Awake helper
  (it takes effect on the next HA sync, or at once with a green press),
  install, then switch it off.
- Before bumping `ESPHOME_VERSION`, compile against the new version
  first (`make build ESPHOME_VERSION=<next>`) — framework migrations
  (like 2026.7's Arduino-as-IDF-component) surface there.
- Reflash/power loss clears RTC memory; graphs repopulate on the next
  fetch (or in device-only mode, refill at one sample per wake).
- The 7-day view needs HA recorder retention ≥ 7 days (default 10).
- The device must be added to HA's ESPHome integration manually by IP —
  discovery rarely catches a sleeping device.
- Radio off makes the device unreachable from HA; green button recovers.
- Three consecutive wakes without a Wi-Fi connection (wrong password,
  AP gone) stop further attempts to save the battery; any real reboot —
  power cycle, reset, OTA — starts over. The debug page shows the strike
  count.
- Double green press = debug page with the running firmware's
  `git describe` stamp — the quick answer to "did my flash take?".
