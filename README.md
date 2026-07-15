# reTerminal E1001 — ePaper Dashboard

ESPHome firmware for a Seeed reTerminal E1001 (ESP32-S3, 7.5" 800×480 B/W
ePaper) showing a seven-page dashboard fed by Home Assistant: world clocks
(digital and analog), indoor/outdoor/device climate, 24-hour and 7-day
history graphs, and a Persian type-rendering test page. Battery powered:
deep sleep 5 min, awake 60 s per cycle.

## How it works

**ESPHome** generates C++ from the YAML and builds it with PlatformIO for
the ESP32-S3 under the Arduino framework. All custom logic lives in two
headers injected via `includes:` and called from YAML lambdas. ESPHome
builds with PlatformIO's library finder off, so the Arduino libs used by
the fetch path (`WiFi`, `WiFiClientSecure`, `HTTPClient`) are declared
explicitly. Fonts are rasterized at compile time — each TTF is subset to
the exact `glyphs:` list and embedded as 1-bit bitmaps (that's how a
size-260 «نقطه» costs ~7 KB and no runtime font engine exists).

Hardware exercised: SPI ePaper (48 KB framebuffer, blocking ~5 s full
refresh), deep sleep with RTC-timer + ext1 button wake
(`esp_sleep_get_ext1_wakeup_status()` tells which button woke it), 8 KB RTC
slow memory (`RTC_DATA_ATTR`, survives deep sleep — holds page index, both
history windows, cached values; ~6.3 KB used), PCF8563 external RTC with
coin-cell backup (read at boot, written on HA time sync), SHT4x onboard
sensor, ADC battery sense, LEDC buzzer PWM.

Algorithms: streaming JSON scan of HA's history API through a custom
Arduino `Stream` (split on `}`, pending-state reassembly, one 512 B
buffer — no PSRAM needed); fixed-width time-bucket downsampling with
forward-fill and all-or-nothing commit per window; Howard Hinnant
days-from-civil for all calendar math; explicit per-zone DST rules
("n-th/last Sunday of month") instead of newlib's unreliable TZ handling;
the jdf integer algorithm for Gregorian→Solar Hijri; nice-step graph
auto-scaling; Arabic presentation forms (U+FB50–FEFF) in visual order for
shaped Persian without HarfBuzz.

## Files

| File | Purpose |
|------|---------|
| `esphome-reterminal-e1001.yaml` | ESPHome config |
| `reterminal_epaper.h` | Page rendering, RTC-memory state, HA history fetch |
| `jalali.h` | Header-only Gregorian → Solar Hijri conversion |
| `noqte.ttf` | Noqte font (from `../noqte`) for the Persian test page |

All four must sit together in the ESPHome builder folder
(`/addon_configs/5c53de3b_esphome/` on current add-ons).

## Pages

Left/right white buttons cycle; the `Page Auto-Cycle` switch (HA entity,
default on) advances one page per 5-minute timer wake.

1. **World clock (digital)** — Tehran, Berlin, Memphis, Mexico City,
   Vancouver with date and offset relative to the local zone; the local
   zone's row is inverted full-width.
2. **Numbers** — Indoor | Outdoor | Device columns, temperature and
   humidity in large type (`21.4°C` / `48%`). Device = onboard SHT4x.
3. **Temperature — 24 h** — indoor solid, outdoor dashed, `data HH:MM`
   stamp in the legend.
4. **Humidity — 24 h** — same layout.
5. **Combined — 7 days** — temperature and humidity stacked with vertical
   inverted TEMP/HUM titles, one centered legend, shared weekday axis.
6. **Analog clocks** — five dials (3+2), thick hands, long quarter markers,
   offsets in the labels, local dial inverted.
7. **Noqte test** — «نقطه» full page in the Noqte font via presentation
   forms.

An inverted status bar sits on every page: Solar Hijri date left,
Gregorian weekday/date/time centered, battery right.

## Buttons

| Trigger | Action |
|---------|--------|
| Left white (GPIO5) | previous page |
| Right white (GPIO4) | next page |
| Green (GPIO3) | force history fetch + redraw |
| HA `button.*` entities | same three actions, emulated (device must be awake) |

All three physical buttons wake the device from deep sleep; page flips
redraw instantly from cached values, green waits for connectivity and
fetches. A short beep acknowledges every press. A 2 s boot gate suppresses
the phantom `on_press` from the wake itself.

## Data flows

- **HA → device (native API, port 6053, Noise-encrypted, HA initiates):**
  live sensor states and time. Time is requested explicitly each wake
  (`id(ha_time).update()`) because the platform's 15-minute poll never
  fires within a wake window; the first sync also starts the PCF8563.
  Requires the device to be added to HA's ESPHome integration (by IP —
  discovery rarely catches a sleeping device).
- **Device → HA (REST, `http://<ha_http_ip>:8123`, device initiates):**
  every 3 h (or on green press) the device pulls 24-hour and 7-day history
  for the four entities via
  `/api/history/period/...?minimal_response&no_attributes` with a Bearer
  token. Windows commit independently; `deep_sleep.prevent/allow` brackets
  the work so sleep cannot truncate the fetch or the final redraw.
  Requires a firewall pinhole: reTerminal IP → HA IP, TCP 8123 (device on
  the IoT VLAN, HA on LAN; give the device a DHCP reservation and order
  the rule above the IoT→LAN block).

## Secrets (`secrets.yaml` next to the config)

```yaml
wifi_password: "..."
ota_password: "..."
api_encryption_key: "..."      # openssl rand -base64 32
ha_http_ip: "192.168.x.x"      # HA LAN IP (firewall pinhole target)
ha_api_token: "..."            # long-lived token of a dedicated NON-ADMIN HA user
```

## Operational notes

- OTA only works while awake — press any button, then Install within 60 s.
- Reflash/power loss clears RTC memory; graphs repopulate on the next
  fetch (up to 3 h, or press green).
- The 7-day view needs HA recorder retention ≥ 7 days (default 10).
- Validation warnings about GPIO3/45 (strapping) and GPIO19/20
  (USB-Serial-JTAG) are inherent to Seeed's board wiring and harmless.
- Verified: `esphome config` valid and full firmware compile clean
  (RAM ~18 %, flash ~69 %). Screen coordinates are tuned blind — expect
  small nudges after seeing real renders.
