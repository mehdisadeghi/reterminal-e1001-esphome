#!/usr/bin/env python3
"""Have the ESPHome Device Builder compile and OTA-install the firmware.

Drives the same builder API Home Assistant's firmware update entity uses,
minus its version gate: the entity only offers an install when the builder
ships a newer ESPHome *version* than the running firmware, so YAML-only
changes never qualify. The builder port is internal to the add-on network,
hence the ssh tunnel.

Usage: release.py <ssh destination> <builder host> <yaml>
Run via `make release` (uv supplies esphome-dashboard-api).
"""

import asyncio
import socket
import subprocess
import sys

import aiohttp
from esphome_dashboard_api import ESPHomeDashboardAPI

UPLOAD_TRIES = 20
UPLOAD_WAIT_S = 15


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


async def wait_port(port, tunnel):
    for _ in range(20):
        if tunnel.poll() is not None:
            sys.exit("ssh tunnel exited before coming up")
        try:
            _, writer = await asyncio.open_connection("127.0.0.1", port)
            writer.close()
            return
        except OSError:
            await asyncio.sleep(0.5)
    sys.exit("ssh tunnel did not come up")


async def run(ssh_dest, builder, yaml):
    port = free_port()
    tunnel = subprocess.Popen(
        ["ssh", "-N", "-o", "ExitOnForwardFailure=yes",
         "-L", f"{port}:{builder}:6052", ssh_dest])
    try:
        await wait_port(port, tunnel)
        async with aiohttp.ClientSession() as session:
            api = ESPHomeDashboardAPI(f"http://127.0.0.1:{port}", session)
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
        tunnel.terminate()


asyncio.run(run(sys.argv[1], sys.argv[2], sys.argv[3]))
