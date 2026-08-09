# Architecture

Working notes behind the README summary. **Nothing here has been compiled or
run**; this describes the design as written, not as verified.

---

## Request flow

```
                            ┌──────────────────────────┐
   HTTP / replay / bench ──▶│  Engine (book registry)  │
                            └────────────┬─────────────┘
                                         │  1. resolve symbol
                                         │     └─▶ UnknownSymbol ──▶ reject
                                         ▼
                            ┌──────────────────────────┐
                            │  Validator (stateless)   │  2. price/qty/tick/band
                            └────────────┬─────────────┘     └─▶ reject + counter
                                         ▼
                    ┌────────────────────────────────────────┐
                    │  IBook  (V0 | V1 | V2 | V3)            │  3. match
                    │  ┌──────────────┐  ┌────────────────┐  │
                    │  │ order index  │  │ price levels   │  │
                    │  │ cid -> order │  │ map or array   │  │
                    │  └──────────────┘  └────────────────┘  │
                    │  ┌──────────────────────────────────┐  │
                    │  │ order pool (V1-V3)               │  │
                    │  └──────────────────────────────────┘  │
                    └────────────────────┬───────────────────┘
                                         ▼
                            ┌──────────────────────────┐
                            │  EventSink               │  fills, acks, cancels
                            └────────────┬─────────────┘
                                         ▼
                            ┌──────────────────────────┐
                            │  InvariantChecker        │  4. paranoid | sampled
                            └────────────┬─────────────┘
                                         │  violation ─▶ abort + dump (paranoid)
                                         │              counter + log (release)
                                         ▼
                                   canonical event log
                                         │
              ┌──────────────────────────┼──────────────────────────┐
              ▼                          ▼                          ▼
     ┌─────────────────┐      ┌────────────────────┐      ┌──────────────────┐
     │ HTTP response   │      │ /stats counters    │      │ replay log file  │
     │ + ladder viewer │      │ + latency hist     │      │  │               │
     └─────────────────┘      └────────────────────┘      └──┼───────────────┘
                                                             │
                                                   ┌─────────▼──────────┐
                                                   │ differential oracle│
                                                   │ reference_matcher  │
                                                   │ validate_fills     │
                                                   └────────────────────┘
```

Validation runs **before** the engine so that the matching path can assume its
inputs are structurally sane and spend its branches on real work. The duplicate
client-order-id check is the exception: it needs the order index, which lives
inside the book, so it happens there.

---

## The four implementations

All four implement `IBook` and must produce byte-identical event logs. They
differ only in how they store price levels and orders.

| | Price ladder | Orders | Order index | Top of book |
|---|---|---|---|---|
| **V0** | `std::map<Price, std::list<Order>>` | by value in the list | `unordered_map` → list iterator | `map.begin()` / `prev(end())` |
| **V1** | `std::map<Price, IntrusiveList>` | pooled, intrusive links | `unordered_map` → `Order*` | `map.begin()` / `prev(end())` |
| **V2** | `vector<IntrusiveList>` indexed by tick | pooled, intrusive links | `unordered_map` → `Order*` | two-level bitmap scan |
| **V3** | `vector<HotLevel>` indexed by tick | pooled `HotOrder`, packed | `unordered_map` → `HotOrder*` | cached index, bitmap on refill |

The `virtual` dispatch through `IBook` costs one indirect call per operation.
That is deliberate and identical across all four, so the comparison stays
honest; the interesting work happens below that boundary.

---

## Matching rules — the specification

Implemented three times: in the four C++ engines, and independently in
`tools/reference_matcher.py`. When they disagree, one of them is wrong, and
finding out which is the entire purpose of `make diff-test`.

1. A buy crosses a resting sell when `taker_price >= resting_price`. A sell
   crosses a resting buy when `taker_price <= resting_price`. A `MARKET` order
   crosses everything.
2. Trades print at the **resting order's** price. The maker arrived first and
   set the terms.
3. Within a price level, orders are consumed **front to back** in arrival order.
4. A partially filled resting order **keeps its queue position**.
5. Self-trade prevention (`CancelResting`): when the aggressor would fill
   against its own resting order, that resting order is cancelled and matching
   continues. The aggressor is not penalised for the collision.
6. `FOK` decides before emitting anything. If the full quantity is not available
   at crossing prices — excluding the participant's own liquidity — the order is
   rejected with no fills.
7. `POST_ONLY` is rejected outright if it would cross. Equal prices cross.
8. `IOC` and `MARKET` cancel any remainder rather than resting.
9. `LIMIT` rests any remainder.

### Event ordering

Fills first, then a terminal event. For one submit:

```
F,...        zero or more fills, in match order
X,...        interleaved cancels for self-trade-prevented makers
A,... FILLED     if fully filled
A,... NEW|PARTIAL if the remainder rested
X,...            if the remainder was cancelled (IOC/MARKET)
R,... REASON     if rejected (no other events)
```

There is no separate ack for the maker side of a fill: the `F` record carries
both client order ids, so a maker's fill is recorded once, in the same line as
the taker's.

Timestamps are deliberately **absent** from the event log. They are the one
field that legitimately differs between two correct runs, and including them
would make byte-identical replay impossible to assert.

---

## Replace semantics

Implemented as cancel-then-new, with one exception.

| Change | Behaviour |
|---|---|
| Quantity decrease at the same price | In-place, **keeps time priority** |
| Quantity increase | Cancel + new, loses priority |
| Any price change | Cancel + new, loses priority |
| New quantity ≤ already filled | Treated as a cancel |

Shrinking an order takes nothing from anyone queued behind it, so there is no
fairness argument for sending it to the back. Every other amend gives the order
something it did not have — a better price, or more size at the same price — and
must therefore re-queue.

---

## Order pool and intrusive lists

The pool is sized at construction and **never grows**. That is a correctness
constraint, not a performance choice: every resting order is referenced by raw
pointer from its price level and from the order index, and a `std::vector`
reallocation would leave every one of those pointers dangling. Exhaustion is
reported as `BookFull`.

The free list threads through the `next` pointer of free records, so it costs no
extra memory. A free `Order` is not a valid order — its `next` is a free-list
link, not a queue link.

---

## The flat price array, and when it is wrong

V2 and V3 index price levels as `(price - base) / tick_size` into a contiguous
array, with a two-level occupancy bitmap for top-of-book.

This is a good trade when the active price range is **narrow and dense**. It is
a bad trade when it is not:

- Memory is paid for every representable level whether occupied or not:
  ~4 MB for `flat_levels = 65,536` across two sides, against ~16 MB of L3 shared
  across all cores.
- Prices outside `[base, base + levels·tick]` cannot be represented and are
  rejected with `PriceOutsideArray`. V0 and V1 accept them.
  `tests/test_v2_boundary.cpp` asserts this divergence explicitly.
- A trending book walks into cold levels, each a DRAM miss at 53–79 ns — more
  than the entire projected V2 insert.

`docs/expected-performance.md` §5 names this as the optimisation most likely to
underdeliver or regress, and `docs/optimization-log.md` has a slot for measuring
where the crossover actually falls.

---

## Invariants

Checked after every event in paranoid mode, every Nth event in release.

| # | Invariant |
|---|---|
| 1 | The book is never crossed: `best_bid < best_ask` when both sides are populated |
| 2 | Each level's aggregate quantity equals the sum of its resting orders (recomputed by walking) |
| 3 | Each level's order count equals the walked count |
| 4 | Total resting orders equals the order index size |
| 5 | Order index size equals pool `in_use` (V1–V3) |
| 6 | The bitmap and the level array agree about occupancy (V2/V3) |
| 7 | Cached top-of-book indices match a fresh bitmap scan (V3) |
| 8 | Sequence numbers are strictly monotonic |
| 9 | Conservation: cumulative buy-side filled quantity equals sell-side |
| 10 | Notional reconciles: buy-side notional equals sell-side |

Invariants 2, 6 and 7 exist because each checks an **incrementally maintained
derived value** against a freshly computed one. Derived state that is updated at
several call sites is exactly where drift hides, and a check that recomputes it
is the only way to find that drift.

Paranoid mode aborts with a full state dump rather than continuing. A corrupted
book must not keep matching: every subsequent fill would be built on state
already known to be wrong. Release mode increments `invariant_violations` and
logs. Neither uses `assert()`, because `assert()` compiles away under `NDEBUG`
and an integrity check that vanishes in the shipped build is worse than no check
at all — it produces false confidence.

---

## Known limitations

- **Single-threaded.** No locking anywhere. The HTTP server runs exactly one
  worker thread for this reason. A second thread would corrupt the book.
- **Single process, no persistence.** State is lost on restart. There is no
  journal, no snapshot, no recovery.
- **No cross-symbol atomicity.** Each symbol is an independent book. There is no
  way to express an operation spanning two of them.
- **The reference price is fixed at construction.** A real venue recalculates it
  intraday; here the price band and the V2/V3 array window are set once and
  never move.
- **`token` / participant identity is not authentication.** `ParticipantId` is
  whatever the caller claims. Self-trade prevention is therefore only as
  trustworthy as the caller.
- **The order index is a `std::unordered_map` in all four versions.** It
  allocates per insert and chases a bucket pointer. The performance model
  predicts it becomes the dominant cost by V3 — which would make replacing it
  the natural V4, and this project does not contain a V4.
