#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Compare a freshly-produced bench JSON against the committed baseline and
# exit nonzero if msgs_per_sec drifted by more than `--threshold` (default
# 30%) in the slow direction. Used as a CI gate so a refactor cannot land
# silent latency regressions. The threshold is intentionally loose: the
# bench is sensitive to runner load and we are protecting against orders-
# of-magnitude regressions, not noise.

import argparse
import json
import sys
from pathlib import Path


def load(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--current", required=True)
    ap.add_argument("--threshold", type=float, default=0.30,
                    help="max allowed fractional slowdown (0.30 = 30%%)")
    ap.add_argument("--metric", default="msgs_per_sec",
                    help="JSON field to compare (higher=better)")
    args = ap.parse_args()

    base = load(args.baseline)
    cur = load(args.current)
    if args.metric not in base or args.metric not in cur:
        print(f"missing metric {args.metric} in one of the inputs", file=sys.stderr)
        return 2
    bv = float(base[args.metric])
    cv = float(cur[args.metric])
    if bv <= 0:
        print(f"baseline value invalid: {bv}", file=sys.stderr)
        return 2
    delta = (cv - bv) / bv  # positive = faster, negative = slower
    print(f"baseline {args.metric}={bv:.2f} current {args.metric}={cv:.2f} "
          f"delta={delta*100:+.2f}%")
    if delta < -args.threshold:
        print(f"FAIL: regression of {delta*100:.2f}% exceeds "
              f"-{args.threshold*100:.2f}% gate", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
