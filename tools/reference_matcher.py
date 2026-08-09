#!/usr/bin/env python3
"""The differential test oracle: a deliberately slow, obviously-correct matcher.

This is the specification of Matchbook's matching semantics, written a second
time, independently, in a language where the implementation can be read at a
glance. Its only job is to disagree with the C++ engines when one of them is
wrong.

It is slow on purpose. Levels are a plain dict, the best price is found by
calling min() or max() over every key, and orders are Python objects in a list.
Nothing here is clever, because clever is what the C++ side is for and two
implementations that share an optimisation share its bugs.

    python3 tools/reference_matcher.py --input flow.csv --output ref.log

Output is the canonical event format from include/matchbook/events.hpp:

    A,<seq>,<client_id>,<status>
    F,<seq>,<taker_client_id>,<maker_client_id>,<price>,<qty>,<taker_side>
    X,<seq>,<client_id>
    R,<seq>,<client_id>,<reason>
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass, field

NO_PRICE = None

# Must match include/matchbook/types.hpp exactly. A mismatch here produces a
# spurious diff that looks like an engine bug.
MAX_PRICE = (2**63 - 1) // 4
MAX_QTY = (2**63 - 1) // 4


@dataclass
class Order:
    client_id: int
    participant: int
    side: str            # "B" or "S"
    otype: str
    price: int | None
    quantity: int
    filled: int = 0
    status: str = "NEW"

    @property
    def remaining(self) -> int:
        return self.quantity - self.filled


@dataclass
class Book:
    tick: int = 1
    reference: int = 100_000
    band_ticks: int = 10_000

    bids: dict[int, list[Order]] = field(default_factory=dict)
    asks: dict[int, list[Order]] = field(default_factory=dict)
    index: dict[int, Order] = field(default_factory=dict)

    seq: int = 0
    events: list[str] = field(default_factory=list)

    # --- event emission ---------------------------------------------------

    def _ack(self, cid: int, status: str) -> None:
        self.seq += 1
        self.events.append(f"A,{self.seq},{cid},{status}")

    def _reject(self, cid: int, reason: str) -> None:
        self.seq += 1
        self.events.append(f"R,{self.seq},{cid},{reason}")

    def _cancel(self, cid: int) -> None:
        self.seq += 1
        self.events.append(f"X,{self.seq},{cid}")

    def _fill(self, taker: int, maker: int, price: int, qty: int, side: str) -> None:
        self.seq += 1
        word = "BUY" if side == "B" else "SELL"
        self.events.append(f"F,{self.seq},{taker},{maker},{price},{qty},{word}")

    # --- helpers ----------------------------------------------------------

    def side_levels(self, side: str) -> dict[int, list[Order]]:
        return self.bids if side == "B" else self.asks

    def best_bid(self) -> int | None:
        return max(self.bids) if self.bids else None

    def best_ask(self) -> int | None:
        return min(self.asks) if self.asks else None

    @staticmethod
    def crosses(taker_side: str, taker_price: int | None, resting: int) -> bool:
        if taker_price is NO_PRICE:
            return True
        return taker_price >= resting if taker_side == "B" else taker_price <= resting

    def validate(self, o: Order) -> str | None:
        """Mirrors src/validation/validator.cpp. Returns a reason or None."""
        if o.quantity <= 0 or o.quantity > MAX_QTY:
            return "INVALID_QUANTITY"
        if o.otype == "MARKET":
            return "MARKET_ORDER_WITH_PRICE" if o.price is not NO_PRICE else None
        if o.price is NO_PRICE:
            return "LIMIT_ORDER_WITHOUT_PRICE"
        if o.price <= 0 or o.price > MAX_PRICE:
            return "INVALID_PRICE"
        if o.price % self.tick != 0:
            return "PRICE_NOT_ON_TICK"
        span = self.band_ticks * self.tick
        if not (self.reference - span <= o.price <= self.reference + span):
            return "PRICE_BAND_VIOLATION"
        if o.price > MAX_PRICE // o.quantity:
            return "INVALID_QUANTITY"
        return None

    def available_to(self, side: str, limit: int | None, participant: int) -> int:
        """Quantity a taker could consume, excluding its own resting orders."""
        opp = self.asks if side == "B" else self.bids
        prices = sorted(opp) if side == "B" else sorted(opp, reverse=True)
        total = 0
        for px in prices:
            if not self.crosses(side, limit, px):
                break
            for o in opp[px]:
                if o.participant == participant:
                    continue  # self-trade prevention cancels it rather than filling
                total += o.remaining
        return total

    # --- operations -------------------------------------------------------

    def submit(self, o: Order) -> None:
        reason = self.validate(o)
        if reason is not None:
            self._reject(o.client_id, reason)
            return

        if o.client_id in self.index:
            self._reject(o.client_id, "DUPLICATE_CLIENT_ORDER_ID")
            return

        if o.otype == "POST_ONLY":
            opp_best = self.best_ask() if o.side == "B" else self.best_bid()
            if opp_best is not None and self.crosses(o.side, o.price, opp_best):
                self._reject(o.client_id, "POST_ONLY_WOULD_CROSS")
                return

        if o.otype == "FOK":
            if self.available_to(o.side, o.price, o.participant) < o.quantity:
                self._reject(o.client_id, "FOK_UNFILLABLE")
                return

        self.seq += 1  # the engine consumes one sequence number for the taker

        if o.otype != "POST_ONLY":
            self._match(o)

        if o.remaining == 0:
            self._ack(o.client_id, "FILLED")
            return

        if o.otype in ("LIMIT", "POST_ONLY"):
            o.status = "PARTIAL" if o.filled > 0 else "NEW"
            self.side_levels(o.side).setdefault(o.price, []).append(o)
            self.index[o.client_id] = o
            self._ack(o.client_id, o.status)
        else:
            self._cancel(o.client_id)

    def _match(self, taker: Order) -> None:
        opp = self.asks if taker.side == "B" else self.bids
        while taker.remaining > 0 and opp:
            best = min(opp) if taker.side == "B" else max(opp)
            if not self.crosses(taker.side, taker.price, best):
                break

            queue = opp[best]
            while taker.remaining > 0 and queue:
                maker = queue[0]

                if maker.participant == taker.participant:
                    queue.pop(0)
                    self.index.pop(maker.client_id, None)
                    maker.status = "CANCELLED"
                    self._cancel(maker.client_id)
                    continue

                qty = min(taker.remaining, maker.remaining)
                price = maker.price  # trades print at the resting order's price

                maker.filled += qty
                taker.filled += qty
                self._fill(taker.client_id, maker.client_id, price, qty, taker.side)

                if maker.remaining == 0:
                    queue.pop(0)
                    self.index.pop(maker.client_id, None)
                    maker.status = "FILLED"
                else:
                    maker.status = "PARTIAL"

            if not queue:
                del opp[best]

    def cancel(self, client_id: int, participant: int) -> None:
        o = self.index.get(client_id)
        if o is None:
            self._reject(client_id, "UNKNOWN_ORDER")
            return
        if o.participant != participant:
            self._reject(client_id, "UNKNOWN_ORDER")
            return
        if o.status in ("FILLED", "CANCELLED", "REJECTED"):
            self._reject(client_id, "ORDER_ALREADY_TERMINAL")
            return

        o.status = "CANCELLED"
        self._remove(o)
        self._cancel(client_id)

    def _remove(self, o: Order) -> None:
        levels = self.side_levels(o.side)
        queue = levels.get(o.price)
        if queue is not None:
            if o in queue:
                queue.remove(o)
            if not queue:
                del levels[o.price]
        self.index.pop(o.client_id, None)

    def replace(self, orig_id: int, new_id: int, participant: int,
                new_price: int | None, new_qty: int) -> None:
        o = self.index.get(orig_id)
        if o is None:
            self._reject(orig_id, "UNKNOWN_ORDER")
            return
        if o.participant != participant:
            self._reject(orig_id, "UNKNOWN_ORDER")
            return
        if o.status in ("FILLED", "CANCELLED", "REJECTED"):
            self._reject(orig_id, "ORDER_ALREADY_TERMINAL")
            return

        if new_qty <= 0 or new_qty > MAX_QTY:
            self._reject(orig_id, "INVALID_QUANTITY")
            return
        if new_price is not NO_PRICE:
            if new_price <= 0 or new_price > MAX_PRICE:
                self._reject(orig_id, "INVALID_PRICE")
                return
            if new_price % self.tick != 0:
                self._reject(orig_id, "PRICE_NOT_ON_TICK")
                return
            span = self.band_ticks * self.tick
            if not (self.reference - span <= new_price <= self.reference + span):
                self._reject(orig_id, "PRICE_BAND_VIOLATION")
                return

        price_unchanged = new_price is NO_PRICE or new_price == o.price
        # A pure quantity reduction at the same price keeps time priority.
        if price_unchanged and o.filled < new_qty <= o.quantity:
            o.quantity = new_qty
            self._ack(orig_id, o.status)
            return

        old_price, old_side, old_type = o.price, o.side, o.otype
        o.status = "CANCELLED"
        self._remove(o)
        self._cancel(orig_id)

        fresh = Order(
            client_id=new_id,
            participant=participant,
            side=old_side,
            otype=old_type,
            price=old_price if new_price is NO_PRICE else new_price,
            quantity=new_qty,
        )
        self.submit(fresh)


def run_stream(lines, book: Book) -> None:
    for raw in lines:
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(",")
        kind = parts[0]

        if kind == "N":
            _, cid, part, side, otype, price, qty = parts
            book.submit(Order(
                client_id=int(cid),
                participant=int(part),
                side=side,
                otype=otype,
                price=NO_PRICE if price == "-" else int(price),
                quantity=int(qty),
            ))
        elif kind == "C":
            _, cid, part = parts
            book.cancel(int(cid), int(part))
        elif kind == "R":
            _, orig, new, part, price, qty = parts
            book.replace(int(orig), int(new), int(part),
                         NO_PRICE if price == "-" else int(price), int(qty))
        else:
            print(f"reference_matcher: unknown record '{kind}'", file=sys.stderr)


def main() -> int:
    ap = argparse.ArgumentParser(description="Reference order matcher (oracle).")
    ap.add_argument("--input", default="-", help="order stream, or - for stdin")
    ap.add_argument("--output", default="-", help="event log, or - for stdout")
    ap.add_argument("--tick", type=int, default=1)
    ap.add_argument("--reference", type=int, default=100_000)
    ap.add_argument("--band-ticks", type=int, default=10_000)
    ap.add_argument("--final-book", default=None,
                    help="also write the final book state here, for state comparison")
    args = ap.parse_args()

    book = Book(tick=args.tick, reference=args.reference, band_ticks=args.band_ticks)

    src = sys.stdin if args.input == "-" else open(args.input, encoding="utf-8")
    try:
        run_stream(src, book)
    finally:
        if src is not sys.stdin:
            src.close()

    out = sys.stdout if args.output == "-" else open(args.output, "w", encoding="utf-8")
    try:
        for e in book.events:
            print(e, file=out)
    finally:
        if out is not sys.stdout:
            out.close()

    if args.final_book:
        with open(args.final_book, "w", encoding="utf-8") as f:
            # Same canonical form the C++ replay tool writes, so final states
            # can be diffed as literally as the event logs.
            for px in sorted(book.bids, reverse=True):
                qty = sum(o.remaining for o in book.bids[px])
                print(f"BID,{px},{qty},{len(book.bids[px])}", file=f)
            for px in sorted(book.asks):
                qty = sum(o.remaining for o in book.asks[px])
                print(f"ASK,{px},{qty},{len(book.asks[px])}", file=f)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
