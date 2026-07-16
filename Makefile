HOST ?= homeassistant
USER ?= $(shell id -un)
TARGET_DIR ?= /config/esphome
DEVICE ?= reterminal-e1001.local

YAML = esphome-reterminal-e1001.yaml
NAME = reterminal-e1001
FILES = $(YAML)
DIRS = reterminal-e1001
BIN = .esphome/build/$(NAME)/.pioenvs/$(NAME)/firmware.bin

.PHONY: build deploy release test

# Compile locally into an SD-flashable app image. Needs the real
# secrets.yaml beside $(YAML) — its values are baked into the binary.
# Pinned to the device builder's version; newer resolvers also reject the
# bare Arduino library names in the yaml.
ESPHOME_VERSION ?= 2026.6.5
build:
	uvx esphome@$(ESPHOME_VERSION) compile $(YAML)
	@echo "copy $(BIN) to the card as /firmware.bin"

# Copy the builder file set to the HA host (secrets.yaml stays untouched).
# tar-over-ssh: the HA SSH add-on has no rsync, but BusyBox tar is there.
deploy:
	COPYFILE_DISABLE=1 tar cf - $(FILES) $(DIRS) | ssh $(USER)@$(HOST) "mkdir -p $(TARGET_DIR) && tar xvf - -C $(TARGET_DIR)"

# Deploy, then have the ESPHome Device Builder compile and OTA-install the
# firmware. HA's firmware update entity is version-gated and never sees
# YAML-only changes, so this drives the builder's own compile/upload API
# through an ssh tunnel (its port is internal to the add-on network). The
# device must wake for the OTA step (green button or the keep-awake helper);
# the upload retries while it sleeps.
BUILDER ?= 5c53de3b-esphome
release: deploy
	uv run --with esphome-dashboard-api python3 release.py $(USER)@$(HOST) $(BUILDER) $(YAML)

# Host-side checks: pure-logic unit tests + example config validation
test:
	c++ -std=c++17 -DRETERMINAL_HOST_TEST -I reterminal-e1001/reterminal tests/host_test.cpp -o /tmp/rt_test
	/tmp/rt_test
	python3 validate_config.py config.json.example
