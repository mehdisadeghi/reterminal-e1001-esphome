# Local specifics (HA host, ssh user) belong in an untracked GNUmakefile
# that sets them and includes this file.
HOST ?= homeassistant
USER ?= $(shell id -un)

# One config for every unit; each names itself from its MAC at runtime,
# and the ESPHome Builder pins that name when the unit is adopted.
NAME = reterminal-e1001
YAML = esphome-$(NAME).yaml
BIN = .esphome/build/$(NAME)/build/firmware.ota.bin
TARGET_DIR ?= /config/esphome

.PHONY: build flash deploy helpers logs test gen-images gen-version gen-ttf

# Stamp the build with git describe; ESPHOME_PROJECT_VERSION shows in HA
# and on the device's debug page (double green press)
gen-version:
	printf 'esphome:\n  project:\n    name: "Seeed Studio.reTerminal E1001"\n    version: "%s"\n' \
	  "$$(git describe --tags --always --dirty 2>/dev/null || echo dev)" \
	  > reterminal-e1001/version.yaml

# Bake the <city>.png dial faces from $(IMAGES) into the firmware; build
# depends on it so the embedded set always matches the files. The set is
# shared by every unit; a unit's own faces come from its SD card.
IMAGES ?= .
gen-images:
	uv run tools/gen_dial_images.py $(IMAGES)

# Subset the outlines the on-device rasterizer draws the status bar from;
# regenerate after changing the UI strings (the glyph list is generated too)
gen-ttf:
	uv run tools/gen_ttf.py

# Compile locally into an SD-flashable app image. Needs secrets.yaml
# beside $(YAML) (see secrets.yaml.example) — its values are baked in.
# Pinned: newer resolvers reject the bare Arduino library names in the yaml.
ESPHOME_VERSION ?= 2026.8.1
build: gen-images gen-version gen-ttf
	uvx esphome@$(ESPHOME_VERSION) compile $(YAML)
	@echo "copy $(BIN) to the card as /firmware.bin"

# First install of a unit, over USB: a stock unit has neither this
# partition table nor the SD updater, and OTA cannot write either
flash: build
	@test -n "$(PORT)" || { echo "make flash PORT=/dev/cu.usbserial-..."; exit 1; }
	uvx esphome@$(ESPHOME_VERSION) upload $(YAML) --device $(PORT)

# Copy the config and the shared tree, generated files included, into the
# Builder's folder on the HA host: adopted units' configs include it from
# there, so every unit builds the same code. Install or Update All in the
# Builder then flashes them (a unit must be awake: Keep Awake helper).
# tar-over-ssh: the HA SSH add-on has no rsync, but BusyBox tar is there.
deploy: gen-images gen-version gen-ttf
	COPYFILE_DISABLE=1 tar cf - $(YAML) reterminal-e1001 | ssh $(USER)@$(HOST) "mkdir -p $(TARGET_DIR) && tar xf - -C $(TARGET_DIR)"
	@echo "deployed to $(USER)@$(HOST):$(TARGET_DIR); install from the ESPHome Builder"

# A unit's three queued-control helpers, as a package of its own (one file
# per unit keeps their input_* keys apart). UNIT is the name the unit
# shows on its setup screen and debug page.
helpers:
	@test -n "$(UNIT)" || { echo "make helpers UNIT=reterminal-e1001-a1b2c3"; exit 1; }
	sed '/^ *#/!s/@UNIT@/$(subst -,_,$(UNIT))/g' ha-helpers.yaml \
	  | ssh $(USER)@$(HOST) "mkdir -p /config/packages && cat > /config/packages/$(subst -,_,$(UNIT)).yaml"
	@echo "takes effect after: ssh $(USER)@$(HOST) ha core restart"

# One unit's log: over the API while it is awake (green press or Keep
# Awake), or PORT=/dev/cu.usbserial-... for USB, which also shows the boot
logs:
	@test -n "$(UNIT)$(PORT)" || { echo "make logs UNIT=reterminal-e1001-a1b2c3 | PORT=/dev/cu.usbserial-..."; exit 1; }
	uvx esphome@$(ESPHOME_VERSION) logs $(YAML) --device $(or $(PORT),$(UNIT).local)

# Host-side checks: pure-logic unit tests + example config validation
test:
	c++ -std=c++17 -DRETERMINAL_HOST_TEST -I reterminal-e1001/reterminal tests/host_test.cpp -o /tmp/rt_test
	/tmp/rt_test
	uv run tools/validate_config.py config.json.example
