# SD-Card Configuration — Design & Implementation

Air-gapped, layman-friendly configuration: settings are **data, not code**.
Pull the SD card, edit `config.json` on any computer, reinsert — the device
validates, applies, persists, and beeps. No flashing, no network.

**Status: implemented** — directly via the Arduino `SD` library (FAT32 over
the shared SPI bus, CS GPIO14, card-detect GPIO15, power rail GPIO16), no
external component needed. Processing runs at boot and on card insertion.

Firmware updates work from the card too — see "Firmware update" below;
only partition-table changes still need USB.

## File

`/config.json` in the card root. JSON, UTF-8, no comments. See
`config.json.example`. Top-level fields, all optional except `version`:

| Field | Type | Meaning |
|---|---|---|
| `version` | int | schema version, must be `1` |
| `set_time` | ISO-8601 UTC string | one-shot clock set (see below) |
| `home_zone` | int 1..5 | zone slot shown as home (inverted, offset base) |
| `zones` | array (1..5) | world clock zones, replaces the stored list |
| `columns` | array (1..3) | climate columns: `{"label", "source"}` where source is `"dev"` or `"<temp_entity>,<hum_entity>"` |
| `ha_url`, `ha_token` | string | HA REST endpoint |
| `sync_interval_min` | int ≥1 | minutes between HA syncs |
| `refresh_interval_min` | int ≥1 | display refresh cadence: deep-sleep wake interval and held-awake redraw interval |
| `night_refresh_min` | int 0..1440 | refresh cadence while the home zone is inside the night window; 0 = no slowdown |
| `combo_days` | int 1..31 | span of the combined temp/hum page (7 = one week) |
| `start_page` | int ≥1 | page after cold boot |
| `night_from`, `night_to` | int 0..23 | analog dial night-inversion hours |
| `show_pages` | array of 8 bools | per-page visibility |
| `bar_pages` | string | pages showing the status bar, comma-separated (e.g. `"2,3,4,5"`) |

Zone object: either `{"tz": "Asia/Tehran", "label": "Tehran"}` (IANA name
resolved against the tz table, `label` optional) or the manual form:
`city` (ASCII, ≤14 chars — glyph set limit), `std_offset_min` (int, minutes
east of UTC), and optionally `dst_offset_min` plus `dst_start` / `dst_end`
rules `{month, week, dow, hour}` (`week` 5 = last, `dow` 0 = Sunday). This
mirrors the firmware's `Zone` struct — the same rendering code draws any
list of 1–5 zones; layout (row height / dial grid) is computed from the
count.

## Timezone data file

The IANA-name lookup uses an embedded table generated from upstream tzdata
(`tzdata.h`, built by `gen_tzdata.py`, *current* rules only). The same rows
exist as `tzdata.csv` (`name,posix` per line). A `/tzdata.csv` on the
card **overrides** the embedded table at boot — it is
reference data, not configuration, so it lives beside `config.json`, not
inside it. That gives an air-gapped path to tzdata updates: regenerate the
CSV on any computer, copy it to the card, done — no reflash.

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
6. **Backup**: each applied config is copied to the card as
   `/<ISO-8601 date>.config.json` (e.g. `2026-07-15.config.json`), giving a
   dated history of applied configs (yesterday's backup = the previous
   config). Keep the newest **5**; older ones are deleted. Same-day repeats
   overwrite the same backup file.

## `set_time` semantics (air-gapped clock setting)

The value is applied **once**: the firmware remembers the last applied
`set_time` string in NVS and ignores the field unless it changed. Workflow:
write the *near-future* wall-clock time into the file, insert the card,
and at the ack beeps the RTC is set. Precision is "seconds", which is fine
for a wall clock re-synced this way a few times a year.

## Firmware update

`/firmware.bin` on the card — the plain OTA app image from `make build`
(needs the real `secrets.yaml` beside the main yaml; never `*.factory.bin`,
which embeds the bootloader and is USB-only) — is flashed into the passive
app slot and booted. Two short beeps, then the device reboots into the new
image; the card's `config.json` is processed on that next boot.

- Cheap per-wake gate: file size+mtime are compared against an NVS
  fingerprint; only a changed file is read fully, and only a changed CRC is
  flashed. Leaving the card inserted costs one stat per wake.
- `esp_ota_end()` validates the image before the boot slot is switched; a
  truncated or wrong-target file is rejected with one long beep.
- Rollback is app-level (the Arduino bootloader lacks
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`): flashing arms an NVS record, the
  earliest boot hook counts boot attempts while armed, and a completed
  `boot_flow` confirms the image. Three failed attempts switch back to the
  previous slot. Network OTA arms the same guard. The limit: an image that
  crashes before the first boot hook runs is not caught — that class is
  covered by the pre-boot image validation above.
- Partition-table changes still require USB.

## Failure behavior

- No card / no file: nothing happens (normal operation).
- Unreadable/invalid file: one long beep + error page; stored config stays.
- Card full during backup: backup is skipped with a logged warning; the new
  config still applies (availability over bookkeeping).
- Power loss mid-apply: NVS writes are atomic per key set; worst case the
  old config remains and the file is retried next boot (hash unchanged
  guard uses the *stored* config's hash, which only updates after a full
  successful apply).

## History snapshots

Independent of the SD card, history (both windows, int16-packed) is
snapshotted hourly to the 24 MB `hist` flash partition (slot ring, CRC'd,
newest wins) and restored on cold boot — graphs survive power loss and
reflashing. A future SD export (`history.csv`) can reuse the same data for
offline analysis.

## Tooling (`tools/`, host-side)

Each script declares its dependencies inline (PEP 723), so `uv run`
resolves them automatically — no flags, no venv.

- `tools/gen_tzdata.py` — extracts current POSIX rules per IANA zone from upstream
  tzdata into `tzdata.csv` (the data file; copy to the card to override) and
  `tzdata.h` (the embedded fallback). Run after a tzdata release:
  `uv run tools/gen_tzdata.py`, reflash or update the card.
- `tools/validate_config.py` — validates a `config.json` against the same rules
  the firmware enforces (schema, zone/column specs, ranges, IANA names and
  their rule forms via `tzdata.csv`, the 8 KB size cap). No ERROR output =
  the device will accept the file; run it before every card trip:
  `uv run tools/validate_config.py config.json`.
- `tools/set_datetime.py` — writes a fresh `set_time` (now + N seconds, default
  60, compensating card-handling delay) into `config.json`:
  `uv run tools/set_datetime.py config.json 90`. Run right before moving the
  card; the firmware applies each distinct value exactly once.

The air-gapped workflow: edit `config.json` → `set_datetime.py` (if the
clock needs setting) → `validate_config.py` → copy to card → insert → two
short beeps.

## Implementation notes

- Arduino `SD`/`FS` libraries over the shared SPI bus; ESPHome's own SPI
  driver and the SD library run serialized in the main loop.
- JSON parsing via `ArduinoJson` (the `json:` component); every zone and
  column spec is validated with the same parsers the HA entities use —
  any error rejects the whole file (one long beep), nothing is applied.
- Accepted files are converted into the same key=value change-set the HA
  `input_text.reterminal_config` queue uses and applied through one shared
  path; the applied values persist in the entities' NVS storage.
- A content CRC in NVS makes application idempotent: the file is re-applied
  only when its bytes change.
- `set_time` one-shot state is a second NVS preference (last applied epoch).
