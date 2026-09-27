# Local specifics (HA host, ssh user) belong in an untracked GNUmakefile
# that sets them and includes this file.
HOST ?= homeassistant
USER ?= $(shell id -un)

# One config for every unit; each names itself from its MAC at runtime,
# and the ESPHome Builder pins that name when the unit is adopted. A local
# build with dial photos points YAML at an untracked config that includes
# this one and lists them (see commissioning.md).
YAML ?= esphome-reterminal-e1001.yaml

# Local builds take the component and fonts from this tree, not GitHub.
# Pinned: newer resolvers reject the bare Arduino library names in the yaml.
ESPHOME_VERSION ?= 2026.8.1
ESPHOME = uvx esphome@$(ESPHOME_VERSION) -s component_source components -s font_dir reterminal-e1001

.PHONY: build flash helpers logs test gen-ttf

# Subset the outlines the on-device rasterizer draws the status bar from;
# regenerate after changing the UI strings (the glyph list is generated too)
gen-ttf:
	uv run tools/gen_ttf.py

# Compile locally into an SD-flashable app image. Needs secrets.yaml
# beside $(YAML) (see secrets.yaml.example) — its values are baked in.
build: gen-ttf
	$(ESPHOME) compile $(YAML)
	@echo "copy .esphome/build/<name>/build/firmware.ota.bin to the card as /firmware.bin"

# First install of a unit, over USB: a stock unit has neither this
# partition table nor the SD updater, and OTA cannot write either
flash: build
	@test -n "$(PORT)" || { echo "make flash PORT=/dev/cu.usbserial-..."; exit 1; }
	$(ESPHOME) upload $(YAML) --device $(PORT)

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
	$(ESPHOME) logs $(YAML) --device $(or $(PORT),$(UNIT).local)

# Host-side checks: pure-logic unit tests + example config validation
test:
	c++ -std=c++17 -DRETERMINAL_HOST_TEST -I components/reterminal/src/reterminal tests/host_test.cpp -o /tmp/rt_test
	/tmp/rt_test
	uv run tools/validate_config.py config.json.example
