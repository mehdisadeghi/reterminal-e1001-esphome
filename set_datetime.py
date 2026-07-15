#!/usr/bin/env python3
"""Stamp config.json's one-shot set_time for air-gapped clock setting.

The firmware applies each distinct set_time value exactly once, so run this
right before moving the card to the device. The offset (seconds, default 60)
pre-compensates the handling delay between stamping and the device reading
the card; the clock lands within that margin.

    python3 set_datetime.py [config.json] [seconds_ahead]
"""

import json
import sys
from datetime import datetime, timedelta, timezone


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "config.json"
    ahead = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    with open(path) as f:
        doc = json.load(f)
    ts = (datetime.now(timezone.utc) + timedelta(seconds=ahead)).strftime("%Y-%m-%dT%H:%M:%SZ")
    doc["set_time"] = ts
    with open(path, "w") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")
    print(f"{path}: set_time = {ts} (now + {ahead}s); check with validate_config.py")


if __name__ == "__main__":
    main()
