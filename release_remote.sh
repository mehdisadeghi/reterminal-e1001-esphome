#!/bin/sh
# Runs on the HA host (piped by `make release` into a login shell so the
# profile exports SUPERVISOR_TOKEN): asks HA to compile + OTA through the
# ESPHome integration's firmware update entity.

entity="${1:?usage: release_remote.sh <update entity id>}"

if [ -z "$SUPERVISOR_TOKEN" ]; then
  echo "SUPERVISOR_TOKEN is not set in this shell (SSH add-on without hassio API?)" >&2
  exit 1
fi

code=$(curl -s -o /tmp/release_out -w '%{http_code}' -X POST \
  -H "Authorization: Bearer $SUPERVISOR_TOKEN" \
  -H "Content-Type: application/json" \
  -d "{\"entity_id\": \"$entity\"}" \
  http://supervisor/core/api/services/update/install)

if [ "$code" != "200" ]; then
  echo "HA answered HTTP $code:" >&2
  cat /tmp/release_out >&2
  echo >&2
  exit 1
fi
echo "release scheduled: HA is compiling and will OTA the device"
