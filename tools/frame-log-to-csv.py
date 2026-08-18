#!/usr/bin/env python3
"""Convert PipeWire frame timing logs into CSV.

Usage:
    frame-log-to-csv.py krdp.stderr > frames.csv
    some-command 2>&1 | frame-log-to-csv.py > frames.csv
"""

import argparse
import csv
import re
import sys
from collections import OrderedDict
from pathlib import Path
from typing import TextIO


TIMING_LOG = re.compile(r"^LOG FRAME (?P<frame_id>-?\d+),(?P<action>received|pushed|encoded),(?P<time>-?\d+)$")
DAMAGE_LOG = re.compile(r"^LOG FRAME DAMAGE (?P<frame_id>-?\d+),(?P<damage>-?\d+)$")


def parse_log(log: TextIO) -> OrderedDict[str, dict[str, str]]:
    frames: OrderedDict[str, dict[str, str]] = OrderedDict()
    for line in log:
        line = line.rstrip("\n")
        timing = TIMING_LOG.match(line)
        damage = DAMAGE_LOG.match(line)
        if timing:
            frame = frames.setdefault(timing["frame_id"], {})
            frame[timing["action"]] = timing["time"]
        elif damage:
            frame = frames.setdefault(damage["frame_id"], {})
            frame["damage"] = damage["damage"]
    return frames

def nanoseconds_to_milliseconds(nanoseconds: str) -> str:
    """Format an integer nanosecond timestamp as exact decimal milliseconds."""
    value = int(nanoseconds)
    sign = "-" if value < 0 else ""
    milliseconds, remainder = divmod(abs(value), 1_000_000)
    return f"{sign}{milliseconds}.{remainder:06d}"


def main() -> None:
    parser = argparse.ArgumentParser(description="Convert PipeWire frame timing logs to CSV.")
    parser.add_argument("log", nargs="?", type=Path, help="stderr log file (defaults to standard input)")
    args = parser.parse_args()

    if args.log:
        with args.log.open(encoding="utf-8", errors="replace") as log:
            frames = parse_log(log)
    else:
        frames = parse_log(sys.stdin)

    writer = csv.DictWriter(sys.stdout, fieldnames=("frame_id", "damage", "received", "pushed", "encoded"), lineterminator="\n")
    writer.writeheader()
    for frame_id, values in frames.items():
        timestamps = {action: nanoseconds_to_milliseconds(timestamp) for action, timestamp in values.items() if action != "damage"}
        writer.writerow({"frame_id": nanoseconds_to_milliseconds(frame_id), **values, **timestamps})

if __name__ == "__main__":
    main()
