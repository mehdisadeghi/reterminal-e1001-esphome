# reTerminal E1001 — ePaper Dashboard

ESPHome config for a Seeed reTerminal E1001 (ESP32-S3, 7.5" 800×480 B/W
ePaper) showing a four-page dashboard fed by Home Assistant. Battery
powered: deep sleep 5 min / awake 45 s.

## Files

| File | Purpose |
|------|---------|
| `esphome-reterminal-e1001.yaml` | ESPHome config (Arduino framework) |
| `reterminal_epaper.h` | Page rendering, RTC-memory state, HA history fetch — must sit next to the YAML in the ESPHome builder folder |

## Pages

1. **World clock** — Tehran, Berlin, Memphis, Mexico City, Vancouver.
   Computed on-device from the PCF8563 RTC using per-zone POSIX TZ strings
   (DST-correct year-round; Tehran and Mexico City are fixed-offset). Works
   offline.
2. **Big numbers** — indoor (`sensor.indoor_*`) and outdoor (`sensor.outdoor_*`)
   temperature + humidity, battery %, last-update stamp.
3. **Temperature graph — 24 h** — indoor solid, outdoor dashed, grid with
   4-hour ticks, current values in the legend, "as of HH:MM" stamp.
4. **Humidity graph — 24 h** — same layout.

## Buttons

| Button | Action |
|--------|--------|
| Left white (GPIO5) | previous page |
| Right white (GPIO4) | next page |
| Green (GPIO3) | force history fetch + redraw |

All three are ext1 deep-sleep wake pins. On a button wake the firmware reads
which pin fired: page flips redraw immediately from cached values (no wait
for Wi-Fi); green waits for HA and fetches. A short beep acknowledges every
press. Presses while awake do the same via `binary_sensor` handlers (a 2 s
boot gate suppresses the phantom press from the wake itself).

## Data flows

Two independent paths, matching the IoT/LAN segmentation:

- **HA → device (native API, port 6053, HA initiates):** current sensor
  values via four `homeassistant` sensors, plus time sync (written back to
  the RTC). Works without any firewall change.
- **Device → HA (REST, `http://<ha_http_ip>:8123`, device initiates):**
  24 h history for the four entities via
  `/api/history/period/<start>?filter_entity_id=...&minimal_response&no_attributes`,
  Bearer-token auth. Requires a firewall pinhole: reTerminal IP → HA IP,
  TCP 8123 (give the device a DHCP reservation; order the rule above the
  IoT→LAN block).

History is fetched every 3 h, or on green button, or when no data exists
yet. Responses are parsed with a streaming scanner (no full-body buffer, no
PSRAM needed), bucketed into 192 points (7.5 min each), forward-filled, and
committed all-or-nothing so a partial failure keeps the previous data.

## State & persistence

Page index, graph buffers, cached sensor values, and the last-fetch
timestamp live in RTC slow memory: they survive deep sleep, and are cleared
only by reflash/power loss (the next fetch repopulates the graphs). Display
`update_interval` is `never`; each wake ends in exactly one explicit
refresh.

## Secrets (`secrets.yaml` next to the config)

```yaml
wifi_password: "..."
ota_password: "..."
api_encryption_key: "..."      # openssl rand -base64 32
ha_http_ip: "192.168.x.x"      # HA LAN IP (firewall pinhole target)
ha_api_token: "..."            # long-lived token of a dedicated NON-ADMIN HA user
```

## Deployment notes

- Copy both files into the ESPHome builder folder
  (`/addon_configs/5c53de3b_esphome/` on current add-ons, `/config/esphome/`
  on older ones).
- OTA only works while the device is awake — press any button, then Install
  within the 45 s window.
- Graph pages show "No history yet" until the first successful fetch.
- `esphome: libraries:` must keep `WiFi`, `WiFiClientSecure`, `HTTPClient`;
  ESPHome builds with PlatformIO's library finder off.
- Verified: `esphome config` valid, full firmware compile clean
  (RAM 18 %, flash 69 %). Screen layout coordinates are untested on real
  hardware and may need nudging.
