HOST ?= homeassistant
USER ?= $(shell id -un)
TARGET_DIR ?= /config/esphome
DEVICE ?= reterminal-e1001.local
# ESPHome add-on container name on HAOS (official add-on slug)
ADDON ?= addon_5c53de3b_esphome

YAML = esphome-reterminal-e1001.yaml
FILES = $(YAML) partitions.csv reterminal.h noqte.ttf vazirmatn.ttf \
        fa_glyphs.yaml fa_title_glyphs.yaml
DIRS = packages reterminal

.PHONY: deploy release test

# Copy the builder file set to the HA host (secrets.yaml stays untouched).
# tar-over-ssh: the HA SSH add-on has no rsync, but BusyBox tar is there.
deploy:
	COPYFILE_DISABLE=1 tar cf - $(FILES) $(DIRS) | ssh $(USER)@$(HOST) "mkdir -p $(TARGET_DIR) && tar xvf - -C $(TARGET_DIR)"

# Deploy, then compile + install from within the ESPHome add-on container.
# Needs the SSH add-on with protection mode off (docker access), and the
# device awake for the OTA step (press a button first).
release: deploy
	ssh $(USER)@$(HOST) "docker exec $(ADDON) esphome run --no-logs --device $(DEVICE) $(TARGET_DIR)/$(YAML)"

# Host-side checks: pure-logic unit tests + example config validation
test:
	c++ -std=c++17 -DRETERMINAL_HOST_TEST -I reterminal tests/host_test.cpp -o /tmp/rt_test
	/tmp/rt_test
	python3 validate_config.py config.json.example
