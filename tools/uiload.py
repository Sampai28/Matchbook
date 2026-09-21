#!/usr/bin/env python3
"""Live load driver for the UI performance comparison.

Consumes the CSV that tools/flowgen.py emits and replays it against the running
server over HTTP, at a configurable rate. Reusing flowgen rather than inventing
a second generator means the UI is driven by exactly the stream the differential
oracle already validates the engine against — same seed, same operations, same
book dynamics.

Standard library only, matching the rest of tools/: no pip install, so this
runs anywhere python3 does.

    python3 tools/flowgen.py --count 20000 --seed 7 > /tmp/flow.csv
    python3 tools/uiload.py --input /tmp/flow.csv --rate 400 --duration 20

Rate is best-effort. If the server cannot keep up the driver reports the rate it
actually achieved rather than silently falling behind, because a comparison run
at an unknown rate is not a comparison.
"""

from __future__ import annotations

import argparse
import json
import sys
import threading
import time
import urllib.error
import urllib.request

SIDE = {"B": "BUY", "S": "SELL"}


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Replay generated order flow over HTTP.")
    p.add_argument("--input", required=True, help="CSV from tools/flowgen.py")
    p.add_argument("--url", default="http://localhost:8080")
    p.add_argument("--symbol", default="MBK")
    p.add_argument("--rate", type=float, default=200.0,
                   help="target operations per second (0 = as fast as possible)")
    p.add_argument("--duration", type=float, default=15.0,
                   help="stop after this many seconds")
    p.add_argument("--timeout", type=float, default=5.0)
    # A single synchronous urllib round trip caps out around 100 ops/s, which
    # is nowhere near enough to stress a browser. The bottleneck is the
    # driver's request latency, not the engine, so the fix is concurrency here.
    # The server serialises on its own mutex regardless, so this raises offered
    # load without changing what the engine does with it.
    p.add_argument("--workers", type=int, default=8,
                   help="concurrent HTTP senders")
    p.add_argument("--quiet", action="store_true")
    return p


def post_order(url: str, payload: dict, timeout: float) -> bool:
    body = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        f"{url}/order", data=body,
        headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            response.read()
            return True
    except urllib.error.HTTPError as exc:
        # A 422 is a rejected order, which is a normal outcome for generated
        # flow — POST_ONLY that would cross, FOK that cannot fill. It is a
        # delivered message, so it counts.
        exc.read()
        return exc.code == 422
    except (urllib.error.URLError, TimeoutError, OSError):
        return False


def cancel_order(url: str, symbol: str, client_id: int, participant: int,
                 timeout: float) -> bool:
    target = (f"{url}/order/{client_id}"
              f"?symbol={symbol}&participant={participant}")
    req = urllib.request.Request(target, method="DELETE")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            response.read()
            return True
    except urllib.error.HTTPError as exc:
        exc.read()
        # 404 means the order was already filled or cancelled — expected in
        # generated flow, not a transport failure.
        return exc.code in (404, 422)
    except (urllib.error.URLError, TimeoutError, OSError):
        return False


def main() -> int:
    args = build_parser().parse_args()

    with open(args.input, "r", encoding="utf-8") as handle:
        lines = [line.strip() for line in handle if line.strip()]

    if not lines:
        print("no operations in input", file=sys.stderr)
        return 1

    interval = 1.0 / args.rate if args.rate > 0 else 0.0
    started = time.perf_counter()
    deadline = started + args.duration

    counter = threading.Lock()
    state = {"sent": 0, "failed": 0, "index": 0}

    def send_one(line: str) -> bool:
        parts = line.split(",")
        kind = parts[0]
        if kind == "N" and len(parts) >= 7:
            _, client_id, participant, side, otype, price, qty = parts[:7]
            payload = {
                "symbol": args.symbol,
                "clientOrderId": int(client_id),
                "participant": int(participant),
                "side": SIDE.get(side, "BUY"),
                "type": otype,
                "quantity": int(qty),
            }
            if price != "-":
                payload["price"] = int(price)
            return post_order(args.url, payload, args.timeout)
        if kind == "C" and len(parts) >= 3:
            return cancel_order(args.url, args.symbol, int(parts[1]), int(parts[2]),
                                args.timeout)
        # Replace has no REST endpoint on this server.
        return True

    def worker(slot: int) -> None:
        while time.perf_counter() < deadline:
            with counter:
                index = state["index"]
                state["index"] += 1
            line = lines[index % len(lines)]

            ok = send_one(line)

            with counter:
                state["sent"] += 1
                if not ok:
                    state["failed"] += 1
                sent_now = state["sent"]

            if interval > 0:
                # Schedule against absolute time so a slow request does not
                # permanently shift every subsequent one and quietly lower the
                # achieved rate.
                target = started + sent_now * interval
                slack = target - time.perf_counter()
                if slack > 0:
                    time.sleep(slack)
            void = slot  # noqa: F841 - slot is only for thread naming

    threads = [threading.Thread(target=worker, args=(i,), daemon=True)
               for i in range(max(1, args.workers))]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=args.duration + args.timeout + 5.0)

    elapsed = time.perf_counter() - started
    sent = state["sent"]
    failed = state["failed"]
    achieved = sent / elapsed if elapsed > 0 else 0.0

    if not args.quiet:
        print(f"sent      : {sent}")
        print(f"failed    : {failed}")
        print(f"elapsed   : {elapsed:.2f}s")
        print(f"workers   : {args.workers}")
        print(f"target    : {args.rate:.0f} ops/s" if args.rate > 0 else "target    : unthrottled")
        print(f"achieved  : {achieved:.0f} ops/s")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
