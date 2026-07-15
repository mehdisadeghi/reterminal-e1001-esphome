#!/usr/bin/env python3
"""Validate a reTerminal config.json before copying it to the SD card.

Mirrors the firmware's validation (reterminal_epaper.h): a file that passes
with no ERROR lines is accepted by the device; any ERROR means the device
would reject the whole file (one long beep, nothing applied).

    python3 validate_config.py [config.json] [tzdata.csv]

tzdata.csv defaults to the copy next to this script and is used to
cross-check IANA zone names and their rule forms.
"""

import json
import os
import re
import sys
from datetime import datetime, timezone

MAX_ZONES, MAX_COLS, PAGES = 5, 3, 8
MAX_FILE = 8192  # firmware read cap
LABEL_MAX = 15   # Zone.city / Column.label storage minus NUL

errors, warnings = [], []
err = errors.append
warn = warnings.append

ENTITY = re.compile(r"^[a-z0-9_]+\.[a-z0-9_]+$")
# firmware-supported POSIX forms: fixed offset, or std+dst with two M-rules
POSIX = re.compile(
    r"^(<[^>]+>|[A-Za-z]+)[+-]?\d+(:\d+){0,2}"
    r"((<[^>]+>|[A-Za-z]+)([+-]?\d+(:\d+){0,2})?"
    r",M\d+\.\d+\.\d+(/-?\d+(:\d+)?)?"
    r",M\d+\.\d+\.\d+(/-?\d+(:\d+)?)?)?$"
)


def ascii_label(s):
    return isinstance(s, str) and 1 <= len(s) <= LABEL_MAX and all(32 <= ord(c) < 127 for c in s)


def derived_label(tz_name):
    return tz_name.rsplit("/", 1)[-1].replace("_", " ")


def load_tzdata(path):
    table = {}
    try:
        with open(path, encoding="ascii") as f:
            for line in f:
                name, _, posix = line.strip().partition(",")
                if name and posix:
                    table[name.lower()] = posix
    except OSError:
        warn(f"{path} not found; IANA names not cross-checked")
    return table


def check_rule(rule, where):
    if not isinstance(rule, dict):
        err(f"{where}: object required")
        return
    for key, lo, hi in (("month", 1, 12), ("week", 1, 5), ("hour", 0, 23)):
        v = rule.get(key)
        if not isinstance(v, int) or not lo <= v <= hi:
            err(f"{where}.{key}: integer {lo}..{hi} required")


def check_zone(z, i, tztable):
    where = f"zones[{i}]"
    if not isinstance(z, dict):
        err(f"{where}: object required")
        return None
    if "tz" in z:
        name = z["tz"]
        if not isinstance(name, str) or not name:
            err(f"{where}.tz: non-empty string required")
            return None
        if tztable and name.lower() not in tztable:
            err(f"{where}.tz: unknown IANA zone '{name}'")
        elif tztable and not POSIX.match(tztable[name.lower()]):
            err(f"{where}.tz: '{name}' uses a rule form the firmware cannot parse")
        label = z.get("label") or derived_label(name)
        if not ascii_label(label):
            err(f"{where}: label '{label}' must be 1..{LABEL_MAX} ASCII chars")
        return label
    city = z.get("city")
    if not ascii_label(city or ""):
        err(f"{where}.city: 1..{LABEL_MAX} ASCII chars required")
    std = z.get("std_offset_min")
    if not isinstance(std, int) or not -720 <= std <= 840:
        err(f"{where}.std_offset_min: integer -720..840 required")
    dst_keys = [k for k in ("dst_offset_min", "dst_start", "dst_end") if k in z]
    if dst_keys and len(dst_keys) != 3:
        err(f"{where}: dst_offset_min, dst_start, dst_end must appear together")
    if len(dst_keys) == 3:
        dst = z["dst_offset_min"]
        if not isinstance(dst, int) or not -720 <= dst <= 840:
            err(f"{where}.dst_offset_min: integer -720..840 required")
        check_rule(z["dst_start"], f"{where}.dst_start")
        check_rule(z["dst_end"], f"{where}.dst_end")
    return city


def check_column(c, i):
    where = f"columns[{i}]"
    if not isinstance(c, dict):
        err(f"{where}: object required")
        return
    if not ascii_label(c.get("label", "")):
        err(f"{where}.label: 1..{LABEL_MAX} ASCII chars required")
    src = c.get("source")
    if src == "dev":
        return
    if not isinstance(src, str) or src.count(",") != 1:
        err(f"{where}.source: 'dev' or '<temp_entity>,<hum_entity>' required")
        return
    for part in src.split(","):
        if not ENTITY.match(part):
            err(f"{where}.source: '{part}' is not an entity id")


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "config.json"
    tzpath = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "tzdata.csv")

    raw = open(path, "rb").read()
    if len(raw) > MAX_FILE:
        err(f"file is {len(raw)} bytes; the firmware reads at most {MAX_FILE}")
    try:
        doc = json.loads(raw)
    except ValueError as e:
        print(f"ERROR: not valid JSON: {e}")
        return 1
    if not isinstance(doc, dict):
        print("ERROR: top level must be an object")
        return 1

    tztable = load_tzdata(tzpath)

    if doc.get("version") != 1:
        err("version: must be 1")

    names = []
    if "zones" in doc:
        zones = doc["zones"]
        if not isinstance(zones, list) or not 1 <= len(zones) <= MAX_ZONES:
            err(f"zones: array of 1..{MAX_ZONES} required")
        else:
            names = [check_zone(z, i, tztable) for i, z in enumerate(zones)]

    if "columns" in doc:
        colsv = doc["columns"]
        if not isinstance(colsv, list) or not 1 <= len(colsv) <= MAX_COLS:
            err(f"columns: array of 1..{MAX_COLS} required")
        else:
            for i, c in enumerate(colsv):
                check_column(c, i)

    if "home_zone" in doc:
        hz = doc["home_zone"]
        if not isinstance(hz, str) or not hz:
            err("home_zone: non-empty string required")
        elif names and hz.lower() not in [n.lower() for n in names if n]:
            warn(f"home_zone '{hz}' matches no zone; the device falls back to the first zone")

    for key, lo, hi in (("night_from", 0, 23), ("night_to", 0, 23),
                        ("sync_interval_min", 1, 1440)):
        if key in doc and (not isinstance(doc[key], int) or not lo <= doc[key] <= hi):
            err(f"{key}: integer {lo}..{hi} required")
    if "start_page" in doc:
        sp = doc["start_page"]
        if not isinstance(sp, int) or sp < 1:
            err("start_page: integer >= 1 required")
        elif sp > PAGES:
            warn(f"start_page {sp} > {PAGES}: the device shows the first page instead")

    if "show_pages" in doc:
        spv = doc["show_pages"]
        if not isinstance(spv, list) or len(spv) != PAGES or not all(
                isinstance(b, bool) for b in spv):
            err(f"show_pages: array of exactly {PAGES} booleans required")

    if "bar_pages" in doc:
        bp = doc["bar_pages"]
        if not isinstance(bp, str) or not all("1" <= c <= "8" for c in bp):
            err("bar_pages: string of page digits 1..8 required, e.g. \"2345\"")

    if "ha_url" in doc and not str(doc["ha_url"]).startswith("http"):
        err("ha_url: must start with http")
    if "ha_token" in doc and "REPLACE" in str(doc["ha_token"]):
        warn("ha_token still holds the placeholder value")

    if "set_time" in doc:
        try:
            ts = datetime.fromisoformat(str(doc["set_time"]).replace("Z", "+00:00"))
            if ts < datetime.now(timezone.utc):
                warn("set_time is in the past; if new, it will set the clock backwards "
                     "(run set_datetime.py just before moving the card)")
        except ValueError:
            err("set_time: ISO-8601 UTC timestamp required, e.g. 2026-07-15T12:00:00Z")

    known = {"version", "set_time", "home_zone", "zones", "columns", "ha_url", "ha_token",
             "sync_interval_min", "start_page", "night_from", "night_to", "show_pages",
             "bar_pages"}
    for key in doc:
        if key not in known:
            warn(f"unknown key '{key}' (ignored by the device)")

    for w in warnings:
        print(f"WARNING: {w}")
    for e in errors:
        print(f"ERROR: {e}")
    if errors:
        print(f"{path}: REJECTED ({len(errors)} error(s)) — the device would long-beep")
        return 1
    print(f"{path}: OK — the device would apply this file")
    return 0


if __name__ == "__main__":
    sys.exit(main())
