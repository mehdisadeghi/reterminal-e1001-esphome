HOST ?= homeassistant
USER ?= $(shell id -un)
TARGET_DIR ?= /config/esphome
DEVICE ?= reterminal-e1001.local

YAML = esphome-reterminal-e1001.yaml
FILES = $(YAML)
DIRS = reterminal-e1001

.PHONY: deploy release test

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
