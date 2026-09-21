# Matchbook

A deterministic, single-threaded **limit order book and matching engine** in
C++20 with price-time priority, implemented four times from an obvious baseline
to a cache-tuned version — plus a streaming market data gateway and a React
trading terminal that renders the book live.

## What a limit order book is, and why the latency is hard

An exchange maintains, per instrument, every resting buy and sell order. Buys
rank by price descending, sells ascending, ties broken by arrival time. When an
order arrives willing to trade at a price resting on the other side, the two
match and a trade prints. The algorithm fits on a napkin. The hard part is that
this structure sits in the path of every message the venue receives, must
produce identical output for identical input forever, and is judged on its
**tail** rather than its average — an engine whose median is 40 ns and whose
p99.9 is 40 µs occasionally loses someone a lot of money. The interesting work
is removing *variance*: allocation, cache misses, unpredictable branches.

## The four implementations

All four sit behind one interface (`IBook`), selectable at runtime, and must
produce byte-identical event logs.

| | Price ladder | Orders | Top of book |
|---|---|---|---|
| **V0 naive** | `std::map<Price, std::list<Order>>` | by value, allocates freely | tree ends |
| **V1 pooled** | `std::map<Price, IntrusiveList>` | pre-allocated pool, intrusive links | tree ends |
| **V2 flat** | contiguous array indexed by ticks-from-base | pooled, intrusive | two-level bitmap |
| **V3 tuned** | same, `alignas(64)` levels | packed `HotOrder` | cached index |

## Measured engine results

AMD Ryzen 7 260, WSL2 native, g++ 13.3.0, `-O2 -DNDEBUG`, pinned to one core
with `taskset -c 2`, 200k operations per repetition, 20k warmup discarded,
median of 5. Raw output in `bench/results/`, full analysis in
[`docs/optimization-log.md`](docs/optimization-log.md).

| Version | insert p50 | insert p99 | insert p99.9 | cancel p50 | match p50 | throughput |
|---|---:|---:|---:|---:|---:|---:|
| V0 | 69 ns | 119 ns | 298 ns | 119 ns | 39 ns | 8.18 M/s |
| V1 | 59 ns | 79 ns | 268 ns | 109 ns | 29 ns | 11.44 M/s |
| V2 | 49 ns | **99 ns** | 228 ns | 39 ns | 29 ns | 12.28 M/s |
| V3 | 39 ns | 79 ns | 129 ns | 39 ns | 29 ns | 13.67 M/s |

End to end: **1.77×** on insert p50, **2.31×** on insert p99.9, **3.05×** on
cancel p50. Projected was 3–4×.

Three results contradicted the hypotheses, and they are the useful ones.
**V2's insert p99 regressed 25%** against V1 while its median improved — the
flat array trades a predictable pointer chase for an indexed load that is fast
when warm and a DRAM round trip when cold, so the median wins and the tail
loses. **Match never improved after V1**, though both V2 and V3 predicted their
largest gains there; locating a price level was never the expensive part of
matching. And **V2 won the sparse-book configuration it was predicted to lose**,
by 41% on insert p50, because `std::map` degrades with depth faster than a flat
array does. V3's cancel p50 carries a 179% inter-run spread and should not be
read as a result.

## Quickstart

```bash
docker compose -f docker/docker-compose.yml up --build
```

- Trading terminal — <http://localhost:3000>
- Engine API and the zero-build fallback viewer — <http://localhost:8080>

Tests and the differential oracle:

```bash
make docker-test     # unit tests, replay determinism, Python oracle, fuzz
make ui-test         # Vitest: delta application, gap recovery, validation
make verify          # all of the above
```

Benchmarks run **natively under WSL2, never in Docker** — containerised timing
on a Windows host inherits Hyper-V scheduling jitter that, at tens of
nanoseconds, is the dominant term. `cmake` is not required; the harness builds
with g++ directly:

```bash
g++ -std=c++20 -O2 -DNDEBUG -Iinclude -Isrc -I. \
    src/engine/*.cpp src/engine/*/*.cpp src/validation/*.cpp bench/bench_main.cpp \
    -o build-native/matchbook-bench
MATCHBOOK_BENCH_BIN=build-native/matchbook-bench ./bench/run_bench.sh
```

## The terminal

React 18 + TypeScript in strict mode, Vite, no state library. A depth ladder
with size bars and virtualized rows, order entry with validation mirroring the
engine's gates, a trade tape, and a stats panel reading engine percentiles from
`/stats/engine`.

Market data arrives over Server-Sent Events: a sequence-numbered snapshot on
connect, then incremental deltas. The client detects sequence gaps and
re-snapshots. Wire format is specified in [`docs/protocol.md`](docs/protocol.md).

### Naive vs optimized rendering

The UI ships both render paths and a toggle between them, because the
difference is the point. **Naive** commits to React state on every message and
re-renders the full ladder from a fresh snapshot. **Optimized** applies deltas
to a mutable book outside React, commits once per `requestAnimationFrame`, and
virtualizes rows.

Identical offered load both runs: 24,015 operations replayed at 1,200 ops/s over
20 seconds, against a book of ~50 price levels.

| | naive | optimized |
|---|---:|---:|
| update-to-paint p50 | 764.0 ms | **12.1 ms** |
| update-to-paint p95 | 9,064.9 ms | **600.0 ms** |
| update-to-paint p99 | 9,574.1 ms | **931.1 ms** |
| update-to-paint max | 157,748.9 ms | **3,706.1 ms** |
| dropped frames | 1,160 | **58** |
| longest frame | 14,450.2 ms | **999.5 ms** |
| React commits | 289 | 989 |

The p50 difference is **63×**. The counterintuitive row is commits: the
optimized path committed *more often* and was still vastly faster, because each
commit was cheap. The naive path managed only 289 commits in 20 seconds because
each one was expensive enough to block the next — a 157-second worst-case
update-to-paint is a tab that has stopped responding, not a slow one.

## Design decisions

**SSE, not WebSocket.** Market data here is unidirectional and order entry
already has a REST path with typed errors. WebSocket would add a dependency
(cpp-httplib has no WebSocket support), a hand-written reconnection strategy,
and a second error convention. `EventSource` reconnects for free, and its
`Last-Event-ID` maps onto the protocol's sequence numbers.

**Absolute deltas, not increments.** A level delta carries the new quantity,
not a change to it. Increments compound silently when one is duplicated or
applied to a level the client got wrong; absolute values are idempotent, so
ordering is the only thing the client must get right, and `seq` already
guarantees that.

**rAF batching.** The screen refreshes 60 times a second. Committing more often
than that renders states nobody could have seen, and at 1,200 messages/second
it is the entire cost of the application.

**No state library.** The book is a mutable structure deliberately outside
React; a store would reintroduce the per-update immutability this design exists
to avoid. What remains is one `useState` per committed frame.

**A wider server thread pool with one mutex.** The engine is still lock-free and
single-threaded, but an SSE connection holds its worker for the life of the
stream — with the original `ThreadPool(1)`, the first browser to connect froze
the whole server. The pool is now 8 and every engine call takes
`ServerState::engine_mu`. Streaming threads hold it only to copy a queued
message, never while writing to a socket.

**Slow consumers are dropped, not waited on.** A full subscriber queue drops the
message rather than blocking the publisher, which runs on the thread that just
mutated the book. The drop becomes a sequence gap, which the client is required
to detect and recover from — so it is a supported path, not data loss.

## Known limitations

- **Single-threaded engine**, no locking inside it. The server serialises access.
- **Single process, no persistence.** No journal, no snapshot, no recovery.
- **No cross-symbol atomicity.** Each symbol is an independent book.
- **The reference price is fixed at construction**, so the price band and the
  V2/V3 array window never move intraday.
- **Participant identity is not authentication.**
- **The order index is a `std::unordered_map` in all four versions**, allocating
  per insert. The measurements suggest it is now the dominant term.
- **The V3 ablation is not done** — isolating its four changes needs four
  additional builds the source does not currently support.
- **Benchmarks are laptop measurements** with frequency scaling active. Relative
  deltas within a run are trustworthy; absolute figures drift between sessions.
