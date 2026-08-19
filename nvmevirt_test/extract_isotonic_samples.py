#!/usr/bin/env python3
"""Extract [ISO-SAMPLE] kernel records into fit_isotonic.py CSV input."""

from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path
from typing import TextIO


SAMPLE_PATTERN = re.compile(
    r"\[ISO-SAMPLE\].*?age_ns=(?P<age>\d+).*?"
    r"future_invalid_pages=(?P<gain>\d+).*?horizon_ns=(?P<horizon>\d+)"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", help="dmesg/log file, or - for stdin")
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument(
        "--target-horizon-ns", type=int, default=10_000_000_000,
        help="normalize observations to this horizon (default: 10 seconds)",
    )
    parser.add_argument(
        "--max-horizon-ratio", type=float, default=2.0,
        help="drop samples observed later than this multiple of target (default: 2)",
    )
    args = parser.parse_args()
    if args.target_horizon_ns <= 0:
        parser.error("--target-horizon-ns must be positive")
    if args.max_horizon_ratio < 1:
        parser.error("--max-horizon-ratio must be at least 1")
    return args


def open_input(name: str) -> tuple[TextIO, bool]:
    if name == "-":
        return sys.stdin, False
    return Path(name).open(encoding="utf-8", errors="replace"), True


def main() -> int:
    args = parse_args()
    stream, should_close = open_input(args.log)
    accepted = 0
    dropped = 0
    try:
        with args.output.open("w", newline="", encoding="utf-8") as output:
            writer = csv.writer(output)
            writer.writerow(["age_ns", "future_invalid_pages", "weight"])
            for line in stream:
                match = SAMPLE_PATTERN.search(line)
                if not match:
                    continue
                age = int(match.group("age"))
                gain = int(match.group("gain"))
                horizon = int(match.group("horizon"))
                if horizon > args.target_horizon_ns * args.max_horizon_ratio:
                    dropped += 1
                    continue
                normalized_gain = gain * args.target_horizon_ns / horizon
                writer.writerow([age, f"{normalized_gain:.9g}", 1])
                accepted += 1
    finally:
        if should_close:
            stream.close()

    print(f"wrote {accepted} samples to {args.output}; dropped={dropped}", file=sys.stderr)
    return 0 if accepted else 1


if __name__ == "__main__":
    raise SystemExit(main())
