#!/usr/bin/env python3
"""Diffs two event logs and reports the first divergence with context.

A plain `diff` tells you two files differ. When a matching engine diverges from
its oracle, what you need is the first line where they part company and the ten
lines on either side, because the bug is almost always in the operation just
before -- a fill that was emitted in the wrong order, a level that was not
cleaned up, a sequence number consumed when it should not have been.

    python3 tools/validate_fills.py ref.log v3.log
    python3 tools/validate_fills.py --context 20 ref.log v3.log
"""

from __future__ import annotations

import argparse
import sys


def load(path: str) -> list[str]:
    with open(path, encoding="utf-8") as f:
        return [ln.rstrip("\n") for ln in f if ln.strip() and not ln.startswith("#")]


def describe(line: str) -> str:
    """Turn a canonical event line into something readable at 2am."""
    parts = line.split(",")
    kind = parts[0] if parts else "?"
    try:
        if kind == "A":
            return f"ACK    seq={parts[1]} order={parts[2]} status={parts[3]}"
        if kind == "F":
            return (f"FILL   seq={parts[1]} taker={parts[2]} maker={parts[3]} "
                    f"px={parts[4]} qty={parts[5]} side={parts[6]}")
        if kind == "X":
            return f"CANCEL seq={parts[1]} order={parts[2]}"
        if kind == "R":
            return f"REJECT seq={parts[1]} order={parts[2]} reason={parts[3]}"
    except IndexError:
        return f"MALFORMED {line!r}"
    return f"UNKNOWN {line!r}"


def main() -> int:
    ap = argparse.ArgumentParser(description="Diff two Matchbook event logs.")
    ap.add_argument("expected", help="reference log (the oracle)")
    ap.add_argument("actual", help="log under test")
    ap.add_argument("--context", type=int, default=10,
                    help="lines of context around the divergence")
    ap.add_argument("--quiet", action="store_true",
                    help="print nothing on success")
    args = ap.parse_args()

    try:
        expected = load(args.expected)
        actual = load(args.actual)
    except OSError as e:
        print(f"validate_fills: {e}", file=sys.stderr)
        return 2

    limit = min(len(expected), len(actual))
    first_bad = None
    for i in range(limit):
        if expected[i] != actual[i]:
            first_bad = i
            break

    if first_bad is None and len(expected) == len(actual):
        if not args.quiet:
            print(f"OK: {len(expected)} events identical")
            print(f"    {args.expected}")
            print(f"    {args.actual}")
        return 0

    print("=" * 72)
    print("EVENT LOG DIVERGENCE")
    print("=" * 72)
    print(f"expected : {args.expected}  ({len(expected)} events)")
    print(f"actual   : {args.actual}  ({len(actual)} events)")
    print()

    if first_bad is None:
        # Identical up to the shorter length: one side simply stopped early,
        # which usually means a crash or an early return rather than a matching
        # difference.
        first_bad = limit
        longer, name = ((expected, args.expected) if len(expected) > len(actual)
                        else (actual, args.actual))
        print(f"Logs agree for all {limit} common events, but {name} has "
              f"{len(longer) - limit} more.")
        print("That usually means one side stopped early rather than matching "
              "differently.")
        print()
        for i in range(limit, min(len(longer), limit + args.context)):
            print(f"  extra [{i}] {describe(longer[i])}")
        return 1

    print(f"First divergence at event index {first_bad}:")
    print()
    print(f"  expected: {describe(expected[first_bad])}")
    print(f"           {expected[first_bad]}")
    print(f"  actual  : {describe(actual[first_bad])}")
    print(f"           {actual[first_bad]}")
    print()

    lo = max(0, first_bad - args.context)
    hi = min(limit, first_bad + args.context + 1)
    print(f"Context (events {lo}..{hi - 1}):")
    print(f"{'':>7} {'EXPECTED':<52} {'ACTUAL'}")
    for i in range(lo, hi):
        marker = ">>" if i == first_bad else "  "
        e = expected[i] if i < len(expected) else "<none>"
        a = actual[i] if i < len(actual) else "<none>"
        print(f"{marker} [{i:>4}] {e:<52} {a}")

    print()
    print("Where to look first:")
    print("  * a FILL in a different order    -> price-time priority in the match loop")
    print("  * a missing CANCEL               -> self-trade prevention or level cleanup")
    print("  * an off-by-one sequence number  -> a path consuming a seq it should not")
    print("  * a REJECT with a different code -> validation ordering between the two")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
