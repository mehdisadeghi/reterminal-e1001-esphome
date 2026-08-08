HOST ?= homeassistant
USER ?= $(shell id -un)
TARGET_DIR ?= /config/esphome

# Per-device: each unit has its own esphome-<device>.yaml whose device_name
# substitution matches <device>; shared packages/code/assets live in
# reterminal-e1001/. Builds land in .esphome/build/<device>/, so devices
# never clobber each other: make release DEVICE=reterminal-e1002
DEVICE ?= reterminal-e1001

YAML = esphome-$(DEVICE).yaml
FILES = $(YAML)
DIRS = reterminal-e1001
BIN = .esphome/build/$(DEVICE)/build/firmware.ota.bin

.PHONY: build deploy release test gen-images gen-version gen-ttf

# Stamp the build with git describe; ESPHOME_PROJECT_VERSION shows in HA
# and on the device's debug page (double green press)
gen-version:
	printf 'esphome:\n  project:\n    name: "reterminal.dashboard"\n    version: "%s"\n' \
	  "$$(git describe --tags --always --dirty 2>/dev/null || echo dev)" \
	  > reterminal-e1001/version.yaml

# Bake the <city>.png dial faces from $(IMAGES) into the firmware; build
# and deploy depend on it so the embedded set always matches the files.
# Per-unit sets are just directories: make release DEVICE=x IMAGES=photos/x
IMAGES ?= .
gen-images:
	uv run tools/gen_dial_images.py $(IMAGES)

# Subset the outlines the on-device rasterizer draws the status bar from;
# regenerate after changing the UI strings (the glyph list is generated too)
gen-ttf:
	uv run tools/gen_ttf.py

# Compile locally into an SD-flashable app image. Needs the real
# secrets.yaml beside $(YAML) — its values are baked into the binary.
# Pinned to the device builder's version; newer resolvers also reject the
# bare Arduino library names in the yaml.
ESPHOME_VERSION ?= 2026.7.0
build: gen-images gen-version gen-ttf
	uvx esphome@$(ESPHOME_VERSION) compile $(YAML)
	@echo "copy $(BIN) to the card as /firmware.bin"

# Copy the builder file set to the HA host (secrets.yaml stays untouched).
# tar-over-ssh: the HA SSH add-on has no rsync, but BusyBox tar is there.
deploy: gen-images gen-version gen-ttf
	COPYFILE_DISABLE=1 tar cf - $(FILES) $(DIRS) | ssh $(USER)@$(HOST) "mkdir -p $(TARGET_DIR) && tar xvf - -C $(TARGET_DIR)"
	@echo "deployed to $(USER)@$(HOST):$(TARGET_DIR) (files only; 'make release' compiles and flashes)"

# Deploy, then have the ESPHome Device Builder compile and OTA-install the
# firmware. HA's firmware update entity is version-gated and never sees
# YAML-only changes, so this drives the builder's own compile/upload API
# through HA core's authenticated ingress proxy on port 8123 (the builder
# is ingress-only; ssh is used just to read the deployed secrets and the
# ingress path). "auto" discovers the builder add-on via the supervisor;
# override with its slug to skip that. The device must wake for the OTA
# step (green button or keep-awake); the upload retries while it sleeps.
BUILDER ?= auto
release: deploy
	uv run tools/release.py $(USER)@$(HOST) $(BUILDER) $(YAML)

# Host-side checks: pure-logic unit tests + example config validation
test:
	c++ -std=c++17 -DRETERMINAL_HOST_TEST -I reterminal-e1001/reterminal tests/host_test.cpp -o /tmp/rt_test
	/tmp/rt_test
	uv run tools/validate_config.py config.json.example
