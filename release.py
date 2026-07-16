#!/usr/bin/env python3
"""Have the ESPHome Device Builder compile and OTA-install the firmware.

Drives the same builder API Home Assistant's firmware update entity uses,
minus its version gate (YAML-only changes never qualify as an "update").
The builder is ingress-only: no TCP port, and its ingress listener accepts
only the supervisor — so the calls go through HA core's authenticated
ingress proxy on port 8123, the same channel the builder UI uses in the
browser. ssh (one master, one passphrase) is only used to read the HA
token from the deployed secrets and to look up the builder's ingress path.

Usage: release.py <ssh destination> <builder slug or "auto"> <yaml>
Run via `make release` (uv supplies esphome-dashboard-api).
"""

import asyncio
import json
import subprocess
import sys
import tempfile
import time

import aiohttp
from yarl import URL
from esphome_dashboard_api import ESPHomeDashboardAPI

UPLOAD_TRIES = 20
UPLOAD_WAIT_S = 15
SESSION_RENEW_S = 300  # ingress sessions expire after ~15 min


def start_master(ssh_dest):
    ctrl = tempfile.mkdtemp(prefix="rt-release-") + "/ctrl"
    master = subprocess.Popen(["ssh", "-M", "-N", "-S", ctrl, ssh_dest])
    for _ in range(120):
        if master.poll() is not None:
            sys.exit("ssh master exited before coming up")
        ok = subprocess.run(["ssh", "-S", ctrl, "-O", "check", ssh_dest],
                            capture_output=True)
        if ok.returncode == 0:
            return ctrl, master
        time.sleep(0.5)
    sys.exit("ssh master did not come up")


def stop_master(ctrl, ssh_dest, master):
    subprocess.run(["ssh", "-S", ctrl, "-O", "exit", ssh_dest],
                   capture_output=True)
    master.terminate()


def remote(ctrl, ssh_dest, cmd):
    out = subprocess.run(["ssh", "-S", ctrl, ssh_dest, cmd],
                         capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"'{cmd}' failed on the host: {out.stderr.strip()}")
    return out.stdout


def remote_json(ctrl, ssh_dest, cmd):
    return json.loads(remote(ctrl, ssh_dest, cmd))["data"]


def secret(ctrl, ssh_dest, key):
    line = remote(ctrl, ssh_dest,
                  f"grep '^{key}:' /config/esphome/secrets.yaml | head -1")
    value = line.split(":", 1)[1].replace('"', "").strip() if ":" in line else ""
    if not value:
        sys.exit(f"{key} not found in the deployed secrets.yaml")
    return value


# "auto" = ask the supervisor for the running ESPHome add-on; anything else
# is taken as the add-on slug. Returns the ingress path on HA core.
def builder_ingress(ctrl, ssh_dest, builder):
    if builder == "auto":
        addons = remote_json(ctrl, ssh_dest, "ha addons --raw-json")["addons"]
        started = [a for a in addons
                   if "esphome" in a["slug"] and a["state"] == "started"]
        if not started:
            sys.exit("no running esphome add-on found (ha addons)")
        started.sort(key=lambda a: 0 if a["slug"].endswith("_esphome") else 1)
        builder = started[0]["slug"]
    info = remote_json(ctrl, ssh_dest, f"ha addons info {builder} --raw-json")
    entry = info.get("ingress_entry")
    if not entry:
        sys.exit(f"add-on {builder} has no ingress entry")
    print(f"device builder: {builder} via {entry}")
    return entry


# The REST supervisor proxy allowlists a handful of paths and 401s the
# rest; the frontend reaches /ingress/session via this websocket command,
# which even non-admin tokens may call.
async def ws_api(session, base, token, endpoint, data=None):
    async with session.ws_connect(f"{base}/api/websocket") as ws:
        await ws.receive_json()  # auth_required
        await ws.send_json({"type": "auth", "access_token": token})
        msg = await ws.receive_json()
        if msg.get("type") != "auth_ok":
            sys.exit("HA websocket auth failed — check ha_api_token")
        req = {"id": 1, "type": "supervisor/api",
               "endpoint": endpoint, "method": "post"}
        if data is not None:
            req["data"] = data
        await ws.send_json(req)
        msg = await ws.receive_json()
        if not msg.get("success"):
            sys.exit(f"{endpoint} refused: {msg.get('error')}")
        return msg["result"]


async def renew_session(session, base, token, sid):
    while True:
        await asyncio.sleep(SESSION_RENEW_S)
        await ws_api(session, base, token, "/ingress/validate_session",
                     {"session": sid})


async def run(ssh_dest, builder, yaml):
    ctrl, master = start_master(ssh_dest)
    try:
        token = secret(ctrl, ssh_dest, "ha_api_token")
        ha_ip = secret(ctrl, ssh_dest, "ha_http_ip")
        entry = builder_ingress(ctrl, ssh_dest, builder)
    finally:
        stop_master(ctrl, ssh_dest, master)

    base = f"http://{ha_ip}:8123"
    # unsafe: the default jar silently drops cookies for IP-address hosts
    jar = aiohttp.CookieJar(unsafe=True)
    async with aiohttp.ClientSession(cookie_jar=jar) as session:
        sid = (await ws_api(session, base, token, "/ingress/session"))["session"]
        session.cookie_jar.update_cookies({"ingress_session": sid}, URL(base))
        renew = asyncio.create_task(renew_session(session, base, token, sid))
        try:
            api = ESPHomeDashboardAPI(base + entry, session)
            print(f"compile task started: {yaml}")
            if not await api.compile(yaml, print):
                sys.exit("compile failed")
            print("upload task: OTA (wake the device or enable keep-awake)")
            for i in range(UPLOAD_TRIES):
                if await api.upload(yaml, "OTA", print):
                    print("release done: device flashed over OTA")
                    return
                print(f"upload attempt {i + 1}/{UPLOAD_TRIES} failed; "
                      f"device asleep? retrying in {UPLOAD_WAIT_S}s")
                await asyncio.sleep(UPLOAD_WAIT_S)
            sys.exit("upload never succeeded — device unreachable")
        finally:
            renew.cancel()


asyncio.run(run(sys.argv[1], sys.argv[2], sys.argv[3]))
