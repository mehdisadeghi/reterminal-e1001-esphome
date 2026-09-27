# Commissioning a unit

How to bring a factory-fresh reTerminal E1001 into service. Every unit
runs the same config. A fresh unit names itself from its MAC
(`reterminal-e1001-a1b2c3`); adopting it in the ESPHome Builder pins that
name, which identifies it in HA and names its helpers. Everything else —
zones, pages, columns, language, intervals — is runtime configuration on
the unit, managed from HA. Nothing about a unit goes into this
repository.

## Once per installation

The ESPHome Builder on the HA host builds every unit, fetching this
repository from GitHub. Its `/config/esphome/secrets.yaml` needs the keys
in `secrets.yaml.example` plus `wifi_ssid` and `wifi_password`.

For flashing over USB from this machine, the same file locally:

```sh
cp secrets.yaml.example secrets.yaml     # fill in; gitignored
```

If the HA host is not `homeassistant` or your ssh user there differs
from your local one, put them in an untracked `GNUmakefile`:

```make
HOST = ha.example
USER = root
include Makefile
```

In HA's `configuration.yaml`, load packages (for the helpers):

```yaml
homeassistant:
  packages: !include_dir_named packages
```

## 1. First flash: USB

A stock unit can only be flashed over USB: this build's partition table
differs from the factory one, and neither OTA nor the SD updater (a
feature of this firmware) can write a partition table.

Switch the unit on (side switch) — the USB-UART bridge only enumerates
with the chip running — then:

```sh
ls /dev/cu.usbserial-*
make flash PORT=/dev/cu.usbserial-XXXXXX
```

Or without a local toolchain: in the ESPHome Builder, create a config
that only pulls the package —

```yaml
packages:
  reterminal: github://mehdisadeghi/reterminal-e1001-esphome/esphome-reterminal-e1001.yaml@main
```

— then **Install → Manual download** (factory format) and flash it with
ESPHome Web, or **Plug into this computer** when HA runs over HTTPS. One
such config serves every new unit.

If no port appears or the upload cannot sync, hold the green button
while switching power to force the ROM bootloader, and retry.

## 2. Wi-Fi

The unit shows its setup screen: join the open `reterminal-e1001`
hotspot from a phone and pick your network in the captive portal (or
use Improv over USB). The hotspot only opens while the unit has no
working Wi-Fi.

## 3. First boot: what you will see (all of it is normal)

- **"Waiting for time sync..."** even though Wi-Fi is connected. Time
  comes from Home Assistant, so nothing syncs until the unit is added
  to HA's ESPHome integration in step 4. The screen leaves this state
  seconds after HA connects.
- **`RTC!` in the status bar / `RTC: BAD` on the debug page** (double
  green press). The PCF8563 has never been written, so the first boot
  necessarily reads an implausible time and flags it. The first HA time
  sync writes the RTC and clears the flag. This says nothing about the
  CR1220 backup cell yet — to test the cell, sync once, switch the unit
  fully off (deep sleep doesn't count; the RTC runs from main power),
  unplug USB for a minute, and boot again: correct time with no `RTC!`
  means the cell holds; `RTC!` again means it is dead or the holder is
  empty. A missing cell degrades gracefully — the unit just needs one HA
  sync after every full power loss.
- **Default configuration.** A fresh unit starts with the example
  settings. Its own configuration arrives once it is in HA: the device's
  HA entities, its Config Queue helper, an SD `config.json`, or a Config
  URL (see `sd-config-design.md`).

## 4. Add it to Home Assistant

Right after setup the unit is awake, so HA usually discovers it under
Settings → Devices & services; otherwise add the **ESPHome** integration
by IP, port `6053`, with the `api_encryption_key`. A short green press
wakes a unit that dozed off. Give it a DHCP reservation in the router —
HA tracks it by IP from now on. On connect the unit gets time, redraws,
and writes the RTC.

In the unit's ESPHome integration options, enable "Allow the device to
perform Home Assistant actions" — the Config Queue clears itself as its
acknowledgement through it.

## 5. Adopt it in the ESPHome Builder

While the unit is awake, the Builder lists it as **Discovered** →
**Adopt**. The Builder writes `/config/esphome/<unit>.yaml`, which pins
the unit's name and pulls this repository's config as a remote package.
From then on the Builder shows the unit, its logs, and installs it.

Dial photos go into that file, per unit or shared through an `!include`:

```yaml
reterminal:
  dial_images:
    berlin: http://homeassistant.local:8123/local/dials/berlin.png
```

## 6. Helpers

Double green press shows the debug page; its first row is the unit's
name.

```sh
make helpers UNIT=reterminal-e1001-a1b2c3
ssh $USER@homeassistant ha core restart
```

This installs `input_button.reterminal_e1001_a1b2c3_press`,
`input_boolean.…_keep_awake` and `input_text.…_config` as the package
`/config/packages/reterminal_e1001_a1b2c3.yaml`. Helpers cannot be
linked to a device, so give each unit a dashboard card holding its
battery, its helpers and its controls.

## Updates

Push to the repository; then in the ESPHome Builder, **Install** per
unit or **Update All** — every build fetches the current source. A
sleeping unit cannot receive an OTA: switch on its Keep Awake helper
first (it takes effect on the unit's next HA sync, or at once with a
green press), and off again afterwards.

## Watching units

- **In HA:** each unit is its own device; entity ids carry its name,
  e.g. `sensor.reterminal_e1001_a1b2c3_battery_level`.
- **In the Builder:** online state and logs per unit (online only while
  awake).
- **Logs from here:** `make logs UNIT=reterminal-e1001-a1b2c3` over the
  API while the unit is awake; `PORT=/dev/cu.usbserial-XXXXXX` reads USB
  and also shows the boot.
- **On the unit:** double green press opens the debug page — name,
  firmware version, Wi-Fi and its strike count, battery, RTC.
