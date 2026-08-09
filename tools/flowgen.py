#!/usr/bin/env python3
"""Seeded order-flow generator.

Emits a deterministic stream of order operations that both the C++ engines and
tools/reference_matcher.py consume. Same seed, same bytes, every time -- that is
what makes the differential test a test rather than a coincidence.

STREAM FORMAT (one operation per line, CSV, no header)

    N,<client_id>,<participant>,<side>,<type>,<price>,<qty>
    C,<client_id>,<participant>
    R,<orig_client_id>,<new_client_id>,<participant>,<price>,<qty>

  side  : B | S
  type  : LIMIT | MARKET | IOC | FOK | POST_ONLY
  price : integer, or "-" for MARKET and for "leave unchanged" on a replace

Chosen over JSON because it parses with str.split() on the Python side and
strtoll on the C++ side, with no dependency and no ambiguity about number
formatting.

    python3 tools/flowgen.py --count 50000 --seed 42 > /tmp/flow.csv
"""

from __future__ import annotations

import argparse
import random
import sys

SIDES = ("B", "S")


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Generate deterministic order flow.")
    p.add_argument("--count", type=int, default=10_000,
                   help="number of operations to emit")
    p.add_argument("--seed", type=int, default=42,
                   help="RNG seed; identical seeds produce identical streams")
    p.add_argument("--reference", type=int, default=100_000,
                   help="reference price the flow clusters around")
    p.add_argument("--tick", type=int, default=1, help="tick size")
    p.add_argument("--participants", type=int, default=8,
                   help="distinct participant ids, used to exercise self-trade prevention")

    p.add_argument("--cancel-ratio", type=float, default=0.20,
                   help="fraction of operations that are cancels")
    p.add_argument("--replace-ratio", type=float, default=0.05,
                   help="fraction of operations that are replaces")
    p.add_argument("--aggressive-ratio", type=float, default=0.25,
                   help="fraction of new orders priced to cross the book")

    p.add_argument("--market-ratio", type=float, default=0.03)
    p.add_argument("--ioc-ratio", type=float, default=0.05)
    p.add_argument("--fok-ratio", type=float, default=0.02)
    p.add_argument("--post-only-ratio", type=float, default=0.05)

    p.add_argument("--cluster-sigma", type=float, default=8.0,
                   help="std-dev in ticks of passive price clustering around reference")
    p.add_argument("--max-qty", type=int, default=500)
    p.add_argument("--out", type=str, default="-", help="output file, or - for stdout")
    return p


def choose_type(rng: random.Random, args: argparse.Namespace) -> str:
    """Pick an order type from the configured mix.

    Ratios are treated as a cumulative distribution rather than being normalised,
    so a caller who over-specifies simply gets fewer plain LIMIT orders instead
    of a silent rescaling of everything else.
    """
    r = rng.random()
    if r < args.market_ratio:
        return "MARKET"
    r -= args.market_ratio
    if r < args.ioc_ratio:
        return "IOC"
    r -= args.ioc_ratio
    if r < args.fok_ratio:
        return "FOK"
    r -= args.fok_ratio
    if r < args.post_only_ratio:
        return "POST_ONLY"
    return "LIMIT"


def generate(args: argparse.Namespace):
    rng = random.Random(args.seed)
    next_id = 1
    live: list[tuple[int, int]] = []  # (client_id, participant) believed resting

    for _ in range(args.count):
        roll = rng.random()

        # --- cancel -------------------------------------------------------
        if live and roll < args.cancel_ratio:
            idx = rng.randrange(len(live))
            cid, part = live.pop(idx)
            yield f"C,{cid},{part}"
            continue

        # --- replace ------------------------------------------------------
        if live and roll < args.cancel_ratio + args.replace_ratio:
            idx = rng.randrange(len(live))
            cid, part = live[idx]
            new_id = next_id
            next_id += 1
            # A replace either reprices or resizes. Repricing loses time
            # priority in the engine; resizing downward keeps it. Both paths
            # need coverage.
            if rng.random() < 0.5:
                offset = int(rng.gauss(0, args.cluster_sigma))
                price = args.reference + offset * args.tick
                price = max(args.tick, price - (price % args.tick))
                qty = rng.randint(1, args.max_qty)
                yield f"R,{cid},{new_id},{part},{price},{qty}"
            else:
                qty = rng.randint(1, args.max_qty)
                yield f"R,{cid},{new_id},{part},-,{qty}"
            live[idx] = (new_id, part)
            continue

        # --- new order ----------------------------------------------------
        cid = next_id
        next_id += 1
        part = rng.randrange(1, args.participants + 1)
        side = rng.choice(SIDES)
        otype = choose_type(rng, args)

        if otype == "MARKET":
            price_field = "-"
        else:
            aggressive = rng.random() < args.aggressive_ratio
            if aggressive:
                # Priced through the touch so it takes liquidity.
                reach = rng.randint(1, 12)
                offset = reach if side == "B" else -reach
            else:
                # Passive: clustered near the reference on the correct side.
                spread = abs(rng.gauss(0, args.cluster_sigma)) + 1
                offset = -spread if side == "B" else spread
            price = args.reference + int(offset) * args.tick
            price = max(args.tick, price)
            price -= price % args.tick
            if price <= 0:
                price = args.tick
            price_field = str(price)

        qty = rng.randint(1, args.max_qty)
        yield f"N,{cid},{part},{side},{otype},{price_field},{qty}"

        # Only order types that can rest are tracked as cancel/replace targets.
        # MARKET, IOC and FOK never rest, so cancelling them would only ever
        # exercise the unknown-order rejection path.
        if otype in ("LIMIT", "POST_ONLY"):
            live.append((cid, part))
            if len(live) > 20_000:
                live.pop(0)


def main() -> int:
    args = build_parser().parse_args()

    for name, value in (("cancel-ratio", args.cancel_ratio),
                        ("replace-ratio", args.replace_ratio),
                        ("aggressive-ratio", args.aggressive_ratio)):
        if not 0.0 <= value <= 1.0:
            print(f"error: --{name} must be between 0 and 1", file=sys.stderr)
            return 2

    out = sys.stdout if args.out == "-" else open(args.out, "w", encoding="utf-8")
    try:
        for line in generate(args):
            print(line, file=out)
    finally:
        if out is not sys.stdout:
            out.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
