# SD-Card Configuration — Design

Air-gapped, layman-friendly configuration: settings are **data, not code**.
Pull the SD card, edit `config.json` on any computer, reinsert — the device
validates, applies, persists, and beeps. No flashing, no network.

## Feasibility

Stock ESPHome has **no SD-card filesystem support**. The card slot on the
reTerminal E1001 hangs off the shared SPI bus (own CS, card-detect on
GPIO15, power rail on GPIO16), which is exactly what community external
components support (e.g. SPI `sd_card` components). Implementation requires
adopting one such external component; everything below is designed to be
independent of which one.

Firmware *logic* updates still need USB (esptool / ESPHome Web on a
desktop). A custom SD-OTA step (reading `firmware.bin` into the inactive
OTA partition) is possible later but out of scope here.

## File

`/config.json` in the card root. JSON, UTF-8, no comments. See
`config.json.example`. Top-level fields, all optional except `version`:

| Field | Type | Meaning |
|---|---|---|
| `version` | int | schema version, must be `1` |
| `set_time` | ISO-8601 UTC string | one-shot clock set (see below) |
| `home_zone` | string | city name; must match one entry in `zones` |
| `zones` | array (1..5) | world clock zones, replaces the built-in list |
| `wake_time_s` | int 10..300 | idle seconds before sleep |
| `start_page` | int ≥1 | page after cold boot |
| `night_from`, `night_to` | int 0..23 | analog dial night-inversion hours |
| `device_only` | bool | climate pages from the onboard sensor only |

Zone object: `city` (ASCII, ≤14 chars — glyph set limit), `std_offset_min`
(int, minutes east of UTC), and optionally `dst_offset_min` plus
`dst_start` / `dst_end` rules `{month, week, hour}` where `week` 1..4 is the
n-th Sunday and `5` the last Sunday of the month. This mirrors the firmware's
`Zone` struct — the same rendering code draws any list of 1–5 zones; layout
(row height / dial grid) is computed from the count.

## Apply algorithm

1. **Trigger**: card-detect edge (GPIO15) plus a check on every boot. A
   file is considered *new* when its `version` is valid and its content hash
   differs from the stored one.
2. **Read** `/config.json` (size cap: 8 KB). Parse JSON.
3. **Validate everything before touching anything** — schema version,
   ranges, zone count, `home_zone` resolves, DST rules complete. Any error
   rejects the whole file: nothing is applied, the error is shown on a
   dedicated page (field name + reason), and the device emits one long beep.
4. **Apply**: copy into the runtime structures, persist to NVS (so the card
   can be removed), redraw.
5. **Acknowledge**: two short beeps = applied.
6. **Backup**: before overwriting the stored config, write the previous
   one back to the card as `/<ISO-8601 date>.config.json`
   (e.g. `2026-07-15.config.json`). Keep the newest **5** backups; delete
   older ones. Same-day repeats overwrite the same backup file.

## `set_time` semantics (air-gapped clock setting)

The value is applied **once**: the firmware remembers the last applied
`set_time` string in NVS and ignores the field unless it changed. Workflow:
write the *near-future* wall-clock time into the file, insert the card,
and at the ack beeps the RTC is set. Precision is "seconds", which is fine
for a wall clock re-synced this way a few times a year.

## Failure behavior

- No card / no file: nothing happens (normal operation).
- Unreadable/invalid file: one long beep + error page; stored config stays.
- Card full during backup: backup is skipped with a logged warning; the new
  config still applies (availability over bookkeeping).
- Power loss mid-apply: NVS writes are atomic per key set; worst case the
  old config remains and the file is retried next boot (hash unchanged
  guard uses the *stored* config's hash, which only updates after a full
  successful apply).

## Firmware changes required (when implemented)

- External component for SPI SD access (read/write/delete files).
- JSON parse + validation module (`ArduinoJson`, already available).
- NVS persistence for the zone list and settings (ESPHome preferences).
- Zone rendering switched from the compile-time array to the runtime list
  (the drawing code is already data-driven; only the source changes).
- Card-detect automation + beep/error-page feedback.
