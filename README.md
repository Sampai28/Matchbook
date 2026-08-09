# Matchbook

A deterministic, single-threaded **limit order book and matching engine** in
C++20 with price-time priority — the core data structure of an electronic
exchange — implemented four times, from an obvious baseline to a cache-tuned
version, with correctness preserved at every step.

---

## What a limit order book is, and why the latency is hard

An exchange maintains, for each instrument, a list of every resting buy order and
every resting sell order. Buys are ranked by price descending, sells by price
ascending, and orders at the same price are ranked by arrival time — this is
*price-time priority*. When a new order arrives willing to trade at a price
already resting on the other side, the two are matched and a trade prints;
whatever is left over either joins the book or leaves, depending on the order
type. The hard part is not the algorithm, which fits on a napkin. The hard part
is that this data structure sits directly in the path of every message the venue
receives, must produce identical output for identical input forever, and is
judged on its **tail** latency rather than its average: an engine whose median is
40 ns and whose p99.9 is 40 µs occasionally loses someone a lot of money. Jitter
is a fairness problem, so the interesting engineering is in removing sources of
*variance* — allocation, cache misses, unpredictable branches — not just in
reducing the mean.

This project is about that removal, done in measured steps. The point is not that
matching works; it is the discipline of the optimisation journey.

---

## The four implementations

All four sit behind one interface (`IBook`) and are selectable at runtime, so the
tests, the differential oracle, the server and the benchmark all drive the same
API. They must produce **byte-identical event logs**.

| | Price ladder | Orders | Top of book | What it demonstrates |
|---|---|---|---|---|
| **V0 naive** | `std::map<Price, std::list<Order>>` | by value, allocates freely | tree ends | Correctness reference. Written to be obviously right. |
| **V1 pooled** | `std::map<Price, IntrusiveList>` | pre-allocated pool, intrusive links | tree ends | Enqueueing allocates nothing. |
| **V2 flat** | contiguous array indexed by ticks-from-base | pooled, intrusive | two-level occupancy bitmap | Tree descent replaced by an indexed load. |
| **V3 tuned** | same, `alignas(64)` levels | packed `HotOrder`, hot fields in one cache line | cached index | Fewer cache lines touched, fewer mispredicts. |

Each is a self-contained file under `src/engine/`. The diff between
`v0_book.cpp` and `v1_book.cpp` is the entire "remove allocation" change; between
`v1` and `v2`, the entire "flatten the ladder" change.

Design reasoning for each pass is in
[`docs/architecture.md`](docs/architecture.md); the cost model behind the
choices is in [`docs/expected-performance.md`](docs/expected-performance.md).

---

## Order type semantics

| Type | On arrival | Remainder | Rejected when |
|---|---|---|---|
| `LIMIT` (GTC) | matches everything that crosses | **rests** on the book | price invalid, off-tick, outside band |
| `MARKET` | matches at any price | **cancelled** | a price was supplied |
| `IOC` | matches what crosses its limit | **cancelled** | price invalid |
| `FOK` | all-or-nothing, decided before any fill | n/a | full quantity unavailable → `FOK_UNFILLABLE`, **zero fills emitted** |
| `POST_ONLY` | never takes liquidity | rests | would cross → `POST_ONLY_WOULD_CROSS`, zero fills |
| `CANCEL` | removes a resting order | n/a | unknown, terminal, or wrong participant |
| `REPLACE` | see below | n/a | unknown, terminal, or wrong participant |

**Replace** is cancel-then-new, with one exception: a pure quantity *decrease* at
the same price is applied in place and **keeps time priority**. Shrinking an
order takes nothing from anyone queued behind it. Every other amend — a price
change, or more size — gives the order something it did not have, and re-queues.

**Trades print at the resting order's price.** The maker arrived first and set
the terms; the taker accepted them.

**Self-trade prevention** uses a *cancel-resting* policy: when an aggressor would
fill against its own resting order, that resting order is cancelled and matching
continues. The aggressor is not penalised for the collision.

---

## Validation and integrity

Every rejection has a named reason and a counter, all exported by `GET /stats`.

**Inbound gates:** non-positive or overflowing price/quantity · price not a
multiple of tick size · price outside the configured band · unknown symbol ·
duplicate client order id · cancel or replace of an unknown or terminal order ·
self-trade prevention · `MARKET` with a price · `LIMIT` without one.

**Book invariants** — checked after every event in paranoid mode, sampled in
release: book never crossed · per-level aggregate equals the sum of its resting
orders · level counts match · total resting equals order-index size · index size
equals pool usage · bitmap agrees with the level array · cached top-of-book
matches a fresh scan · sequence numbers strictly monotonic · **conservation**
(buy-side filled quantity equals sell-side, and notional reconciles).

Paranoid mode aborts with a full state dump. Release increments
`invariant_violations` and logs. Neither uses `assert()` — an integrity check
that compiles away under `NDEBUG` is worse than no check, because it produces
false confidence.

---

## Quickstart

Two workflows, deliberately separate.

### Build and test — in Docker

Reproducible, and pinned to `gcc:13` so the containerised compiler matches
WSL2's native g++ 13.3.0.

```bash
make docker-test
```

Runs the unit tests, the replay-determinism check, the differential oracle
against the Python reference, and a short fuzz run.

```bash
docker compose -f docker/docker-compose.yml up --build server
```

Serves the ladder viewer and the API on <http://localhost:8080>.

### Benchmark — natively in WSL2

```bash
cmake --preset release
cmake --build --preset release -j
./bench/run_bench.sh
python3 tools/make_report.py
```

`make bench` refuses to run inside a container. Containerised timing on a
Windows host inherits scheduling jitter from Hyper-V and I/O latency from the
filesystem shim; at millisecond resolution that is invisible, but at the
tens-of-nanoseconds resolution this harness works in it is the dominant term, and
the numbers would describe the virtualisation stack rather than the order book.

`perf` is assumed unavailable under the stock WSL2 kernel, so the harness relies
entirely on in-process timing: a calibrated `rdtsc` for per-operation latency and
`std::chrono::steady_clock` for wall time. It pins to one core, discards warmup,
runs five independent repetitions, and reports the median alongside the
inter-run spread — the spread being the error bar that says whether a difference
between two versions means anything.

### Make targets

| Target | Does |
|---|---|
| `build` | configure + compile (release) |
| `test` | Catch2 suite via CTest |
| `diff-test` | Python oracle vs all four engines over 50,000 operations |
| `replay-diff` | byte-identical replay across runs and across versions |
| `fuzz` | validation-layer fuzz target |
| `bench` | the harness, natively |
| `report` | `bench/report.html` from `bench/results/` |
| `serve` | HTTP server + ladder viewer on :8080 |
| `docker-build`, `docker-test` | containerised build and full check suite |

---

## HTTP API

| Endpoint | Purpose |
|---|---|
| `POST /order` | submit — JSON body with `symbol`, `clientOrderId`, `side`, `type`, `quantity`, `price` |
| `DELETE /order/{id}?symbol=X&participant=N` | cancel |
| `GET /book/{symbol}?depth=N` | depth-of-book ladder |
| `GET /stats` | counters, every rejection reason, invariant violations, latency percentiles |
| `GET /healthz` | liveness |
| `GET /` | the static ladder viewer |

The viewer is one HTML page with vanilla JS and no build step, served by the
binary from `web/`. It polls the book, draws a live bid/ask ladder with size
bars, and can submit orders directly.

---

## Correctness scaffolding

**Differential oracle.** `tools/reference_matcher.py` is a deliberately slow,
obviously-correct matcher written independently in Python. `make diff-test`
generates seeded order flow, runs it through the reference and all four C++
engines, and asserts the event logs and final book states are identical.
`tools/validate_fills.py` prints the first divergence with context when they are
not.

**Deterministic replay.** `make replay-diff` replays
`tests/fixtures/recorded_stream.csv` twice through each engine and across all
four, requiring byte-identical output every time. The event log deliberately
carries no timestamps — they are the one field that legitimately differs between
two correct runs, and including them would make this assertion impossible.

**Unit tests** (Catch2 v3) cover price-time priority under partial fills, each
order type's semantics including FOK's all-or-nothing rejection and POST_ONLY's
rejection on cross, cancel/replace of unknown and terminal orders, and V2's
price-array boundary behaviour. Every behavioural test runs against all four
engines.

**Fuzzing.** `tests/fuzz_validation.cpp` builds standalone with a seeded corpus
by default, or with libFuzzer under clang via `-DMATCHBOOK_LIBFUZZER=ON`.

---

## Design decisions

**Why intrusive lists.** An intrusive list stores its `prev`/`next` pointers
inside the element rather than in a node the container allocates. Two
consequences: enqueueing an order allocates nothing, and cancelling is O(1) from
the order's address alone — the index hands back a pointer and the order unlinks
itself, with no search and no iterator to keep valid. The cost is that an order
can be in at most one list at a time, which is exactly true here: an order rests
at one price level or nowhere.

**Why a flat price array, and when it is the wrong choice.** Indexing
`(price - base) / tick` is a subtract, a divide and one load; a `std::map`
descent is roughly seven pointer chases with poor locality and a mispredicted
branch at most of them. That trade is good when the active price range is narrow
and dense. It is bad when it is not: memory is paid for every representable level
whether occupied or not, prices outside the window cannot be represented *at all*
(V2/V3 reject them with `PriceOutsideArray` while V0/V1 accept them —
`tests/test_v2_boundary.cpp` asserts this divergence deliberately), and a
trending book walks into cold levels at DRAM cost. This design suits a liquid
instrument whose tick size puts active trading across hundreds of levels rather
than tens of thousands. That is a real and common case, but it is a precondition,
not a given.

**Why the order pool never grows.** Every resting order is referenced by raw
pointer from its price level and from the order index. A `std::vector`
reallocation would leave all of them dangling. Growth is therefore forbidden, not
unimplemented; exhaustion is reported as `BookFull`.

**Self-trade prevention: cancel-resting.** The alternative — rejecting the
aggressor — punishes a participant for a collision it may not have known about,
and leaves the stale resting order in place to cause the same collision again.

**Why `virtual` dispatch stayed.** One indirect call per operation, paid
identically by all four versions. The benchmark measures a realistic deployment
where the implementation is chosen at runtime, and removing the vtable from V3
alone would flatter it for a reason unrelated to the optimisation being studied.

### Known limitations

- **Single-threaded**, with no locking anywhere. The HTTP server runs exactly one
  worker for this reason; a second would corrupt the book.
- **Single process, no persistence.** No journal, no snapshot, no recovery.
- **No cross-symbol atomicity.** Each symbol is an independent book.
- **The reference price is fixed at construction.** A real venue recalculates it
  intraday; here the price band and the V2/V3 array window never move.
- **Participant identity is not authentication.** `ParticipantId` is whatever the
  caller claims, so self-trade prevention is only as trustworthy as the caller.
- **The order index is a `std::unordered_map` in all four versions**, allocating
  per insert. The cost model predicts it becomes the dominant term by V3, which
  would make replacing it the natural next pass.

---

## Repository layout

```
include/matchbook/       public headers: types, Order, IBook, Engine, events, metrics
src/engine/v0_naive/     baseline: std::map + std::list
src/engine/v1_pool/      pooled orders + intrusive lists
src/engine/v2_flat/      flat price array + occupancy bitmap
src/engine/v3_tuned/     packed records, cached top-of-book, branch hints
src/engine/common/       order pool, intrusive list, two-level bitmap
src/validation/          inbound validation + invariant checker
src/server/              cpp-httplib server, JSON, metrics
web/                     static ladder viewer (no build step)
tests/                   Catch2 suite, fuzz target, replay tool, fixture
bench/                   harness, HdrHistogram, rdtsc, results/
tools/                   flowgen, reference_matcher, validate_fills, make_report
docs/                    architecture, expected-performance, optimization-log, BUILD_NOTES
docker/                  Dockerfile (gcc:13), docker-compose.yml
```

---

## License

Unlicensed personal project. All dependencies are free and open source — Catch2
(BSL-1.0), cpp-httplib (MIT), Plotly (MIT). Nothing here requires an account, an
API key, or a paid service.
