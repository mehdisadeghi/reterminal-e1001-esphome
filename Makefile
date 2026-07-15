HOST ?= homeassistant
USER ?= $(shell id -un)
TARGET_DIR ?= /config/esphome
DEVICE ?= reterminal-e1001.local

YAML = esphome-reterminal-e1001.yaml
FILES = $(YAML) partitions.csv reterminal.h noqte.ttf vazirmatn.ttf \
        fa_glyphs.yaml fa_title_glyphs.yaml
DIRS = packages reterminal

.PHONY: deploy release test

# Copy the builder file set to the HA host (secrets.yaml stays untouched).
# tar-over-ssh: the HA SSH add-on has no rsync, but BusyBox tar is there.
deploy:
	COPYFILE_DISABLE=1 tar cf - $(FILES) $(DIRS) | ssh $(USER)@$(HOST) "mkdir -p $(TARGET_DIR) && tar xvf - -C $(TARGET_DIR)"

# Deploy, then ask Home Assistant to build + OTA-install through the
# ESPHome integration's firmware update entity (the add-on does the work).
# Runs on the HA host via the supervisor's core API proxy; the device must
# be awake for the OTA step (press a button, or use the keep-awake helper).
UPDATE_ENTITY ?= update.reterminal_e1001_firmware
release: deploy
	ssh $(USER)@$(HOST) 'curl -sf -X POST \
	  -H "Authorization: Bearer $$SUPERVISOR_TOKEN" \
	  -H "Content-Type: application/json" \
	  -d "{\"entity_id\": \"$(UPDATE_ENTITY)\"}" \
	  http://supervisor/core/api/services/update/install' \
	  && echo "release scheduled: HA is compiling and will OTA $(DEVICE)"

# Host-side checks: pure-logic unit tests + example config validation
test:
	c++ -std=c++17 -DRETERMINAL_HOST_TEST -I reterminal tests/host_test.cpp -o /tmp/rt_test
	/tmp/rt_test
	python3 validate_config.py config.json.example
