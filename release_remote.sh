#!/bin/sh
# Runs on the HA host (piped by `make release`): asks HA to compile + OTA
# through the ESPHome integration's firmware update entity. Auth: the
# long-lived token from the deployed secrets.yaml, against HA's own API —
# the SSH add-on's SUPERVISOR_TOKEN is not authorized for the core proxy.

entity="${1:?usage: release_remote.sh <update entity id>}"
secrets="/config/esphome/secrets.yaml"

# value of a top-level "key: value" line, quotes and spaces stripped
get() {
  grep "^$1:" "$secrets" | head -1 | cut -d: -f2- | tr -d ' "'
}

token=$(get ha_api_token)
ip=$(get ha_http_ip)
if [ -z "$token" ] || [ -z "$ip" ]; then
  echo "ha_api_token/ha_http_ip not found in $secrets" >&2
  exit 1
fi

code=$(curl -s -o /tmp/release_out -w '%{http_code}' -X POST \
  -H "Authorization: Bearer $token" \
  -H "Content-Type: application/json" \
  -d "{\"entity_id\": \"$entity\"}" \
  "http://$ip:8123/api/services/update/install")

if [ "$code" != "200" ]; then
  echo "HA answered HTTP $code:" >&2
  cat /tmp/release_out >&2
  echo >&2
  exit 1
fi
echo "release scheduled: HA is compiling and will OTA the device"
