> **⚠️ THESE ARE UNVERIFIED PROJECTIONS.** No benchmark has been executed. Every number here is derived from a cost model, not measurement. Replace this entire document with real results after running `make bench`. Do not cite any figure from this file in a résumé, interview, or any external communication.

# Expected performance — a first-principles projection

This document exists because the code was written but never run. Rather than
leave the performance section blank, it works out what the design *should* cost
from the hardware up. That has two uses: it gives the first benchmark run
something to falsify, and where measurement disagrees with the model, the
disagreement is the interesting result.

Nothing here is a measurement. Every number is arithmetic over a cost table.

---

## 1. The cost model

Target: **AMD Ryzen 7 260**, 8 cores / 16 threads, 3.8 GHz base, ~16 MB L3,
64-byte cache lines, Zen-class microarchitecture.

At 3.8 GHz one cycle is **0.263 ns**.

| Event | Cycles | Nanoseconds | Note |
|---|---:|---:|---|
| L1d hit | 4–5 | 1.05–1.32 | |
| L2 hit | ~14 | ~3.7 | |
| L3 hit | 40–50 | 10.5–13.2 | shared, so contended in practice |
| DRAM | 200–300 | 53–79 | commonly quoted as 70–100 ns including TLB effects |
| Branch mispredict | 15–20 | 3.9–5.3 | |
| Integer divide (64-bit, runtime divisor) | 20–40 | 5.3–10.5 | not strength-reducible when the divisor is not a constant |
| Shift / mask / add | 1 | 0.26 | |
| `__rdtscp` | 6–10 | 1.6–2.6 | the measurement instrument itself |
| `clock_gettime` via vDSO | 60–90 | 15–24 | why the harness does not use it per-operation |

**glibc `malloc`/`free` for a small object.** A tcache hit is roughly 15–25
cycles each way once the size-class lookup and the free-list pointer chase are
included, so an allocate-then-free round trip costs **~10–20 ns** when
everything is hot. That is the optimistic case. It degrades sharply when the
tcache is empty and the request falls through to the fastbins or the unsorted
bin, and every allocation also touches a chunk header that is likely to be a
cache miss in a book that has been running for a while. A realistic steady-state
figure for one allocation in a workload that is constantly allocating and
freeing order nodes is **20–35 ns**, with the free costing a further 10–20 ns.

**Red-black tree depth.** `std::map` has depth between `log2(n+1)` and
`2·log2(n+1)`. For the price-level counts that matter:

| Live price levels | Best-case depth | Worst-case depth | Realistic |
|---:|---:|---:|---:|
| 20 | 4.4 | 8.8 | ~6 |
| 40 | 5.4 | 10.7 | ~7 |
| 200 | 7.7 | 15.3 | ~10 |
| 1,000 | 10.0 | 19.9 | ~13 |

Each node is a separate allocation, so consecutive nodes on a descent path are
not adjacent in memory. For a book of ~40 levels the top two or three levels of
the tree stay hot in L1/L2 because every lookup touches them; the leaves are
cold. A reasonable split for a 7-node descent is **2 × L1, 3 × L2, 2 × L3**,
which is `2(1.2) + 3(3.7) + 2(11.8)` = **37 ns** of pure memory latency, plus
the comparisons.

**Branch misprediction on a tree descent.** Each node visit branches on
`key < node->key`, and in a book where prices arrive scattered around the touch
that branch is close to unpredictable. Assume the predictor gets the top two
levels right (they are biased) and misses roughly half of the remaining five:
**2–3 mispredicts × 4.6 ns = 9–14 ns** per descent.

---

## 2. Cache lines touched per operation

| Structure | Size | Lines | Notes |
|---|---:|---:|---|
| `matchbook::Order` (V0–V2) | ~88 B | 2 | hot fields (`price`, `quantity`, `filled`, `next`, `participant`) are spread across both |
| `v3::HotOrder` | ~72 B | 2 allocated, **1 touched** | the matching loop reads only the first 32 B |
| `detail::IntrusiveList` | 32 B | 1 | head, tail, count, aggregate together |
| `v3::HotLevel` | 64 B, `alignas(64)` | 1 | one level never straddles two lines |
| `std::map` node | ~64 B + header | 1–2 | plus an allocator header, and no locality between nodes |

**Matching against N makers at one level:**

- V1/V2 touch **2N** lines: each `Order` straddles two lines and both halves are
  read (price and quantity in one, `next` in the other, depending on layout).
- V3 touches **N** lines in the common case, and fewer when the pool has handed
  out adjacent slots — two 72-byte records can share a line boundary such that
  the prefetcher covers both.

For a 5-maker sweep that is a difference of 5 line fetches. If those lines are
in L2, that is **~18 ns**; if in L3, **~55 ns**. This is the single clearest
quantitative argument for the V3 packing work, and also why the win should be
larger on `match` than on `insert`.

---

## 3. Projections

### V0 — naive (`std::map` + `std::list` + `std::unordered_map`)

**Insert cost decomposition:**

| Term | Estimate |
|---|---:|
| `std::map` descent to the price level (~7 nodes) | 37 ns |
| Mispredicts on the descent | 9–14 ns |
| `std::list::push_back` node allocation | 20–35 ns |
| `std::unordered_map` insert (hash, bucket, node allocation) | 25–40 ns |
| New price level: `std::map` node allocation + rebalance | +30–50 ns, amortised ~10 ns |
| Event construction and sink push | 5–10 ns |

| Metric | Projected range | Dominant term |
|---|---|---|
| insert p50 | **110–180 ns** | three separate allocations |
| insert p99 | **300–600 ns** | allocator falling out of tcache; a rebalance that touches cold nodes |
| cancel p50 | **70–120 ns** | hash lookup + list node free + possible map erase |
| match p50 (5 levels) | **450–800 ns** | 5 × (list node free + map node erase + tree rebalance) |
| throughput | **3–6 M ops/sec** | |

The p99/p50 ratio is the tell here. Allocation has a long tail by construction:
most calls hit the tcache, and the ones that do not are an order of magnitude
worse. V0 should show the widest tail of the four.

### V1 — pooled orders + intrusive lists

Removes the two per-order allocations and the list node. The `std::map` price
ladder is unchanged, so the tree descent and its mispredicts remain.

| Term | Change from V0 |
|---|---|
| List node allocation | **−20 to −35 ns** (eliminated) |
| Order pool acquire | +2–4 ns (pop a free-list head; one likely-hot line) |
| `unordered_map` insert | unchanged, still allocates a node |
| Tree descent | unchanged |
| Cancel: O(1) unlink instead of `list::erase` + free | **−15 to −25 ns** |

| Metric | Projected range | Dominant term |
|---|---|---|
| insert p50 | **75–125 ns** | `std::map` descent — now the largest single item |
| insert p99 | **180–350 ns** | tail narrows sharply; the allocator is no longer in the path |
| cancel p50 | **45–75 ns** | hash lookup |
| match p50 (5 levels) | **220–400 ns** | tree erase per emptied level |
| throughput | **6–10 M ops/sec** | |

**The p99 improvement should exceed the p50 improvement.** Removing allocation
takes a fixed ~25 ns off the median but removes an entire long-tail failure mode.
If the measured p99 does not narrow proportionally more than p50, the model is
wrong about where V0's tail comes from, and that is worth investigating.

### V2 — flat price array + occupancy bitmap

Replaces the tree descent with an index computation and an indexed load.

| Term | Estimate |
|---|---:|
| `price_to_index`: subtract, modulo, divide, bounds check | 7–13 ns (the 64-bit divide dominates) |
| Indexed load of the level | 1.2 ns (L1) to 12 ns (L3) to 60 ns (DRAM, cold level) |
| Bitmap set + summary set | 2–4 ns (two hot lines) |
| `unordered_map` insert | unchanged, 25–40 ns |
| Best-bid/ask via two-level bitmap | 3–6 ns |

Replacing a ~46 ns tree descent (37 ns memory + 9 ns mispredicts) with ~10–20 ns
of index arithmetic and one array load is the largest structural change in the
series.

| Metric | Projected range | Dominant term |
|---|---|---|
| insert p50 | **45–85 ns** | `unordered_map` insert is now the largest item |
| insert p99 | **120–250 ns** | cold-level DRAM misses on a wide book |
| cancel p50 | **35–60 ns** | hash lookup |
| match p50 (5 levels) | **140–260 ns** | bitmap `find_next` per level + maker line fetches |
| throughput | **10–16 M ops/sec** | |

### V3 — hot-path tuning

| Change | Expected effect |
|---|---|
| Power-of-two tick → shift instead of divide | **−5 to −10 ns** per price-to-index, on every insert |
| Cached best-bid/ask index | **−3 to −6 ns** per submit, more on POST_ONLY and FOK |
| Compact `HotOrder` (1 line touched per maker instead of 2) | **−3.7 to −11 ns per maker**, so −18 to −55 ns on a 5-maker sweep |
| `alignas(64)` on `HotLevel` | prevents one level straddling two lines: saves a line fetch on some fraction of accesses |
| Branch hints | **−0 to −4 ns**, and possibly negative — see §5 |

| Metric | Projected range | Dominant term |
|---|---|---|
| insert p50 | **32–65 ns** | `unordered_map` insert, now unambiguously the bottleneck |
| insert p99 | **90–190 ns** | hash collisions and rehash |
| cancel p50 | **28–50 ns** | hash lookup |
| match p50 (5 levels) | **100–200 ns** | maker line fetches |
| throughput | **14–22 M ops/sec** | |

### Summary

| Version | insert p50 | insert p99 | cancel p50 | match p50 (5 lvl) | throughput |
|---|---|---|---|---|---|
| V0 naive | 110–180 ns | 300–600 ns | 70–120 ns | 450–800 ns | 3–6 M/s |
| V1 pooled | 75–125 ns | 180–350 ns | 45–75 ns | 220–400 ns | 6–10 M/s |
| V2 flat | 45–85 ns | 120–250 ns | 35–60 ns | 140–260 ns | 10–16 M/s |
| V3 tuned | 32–65 ns | 90–190 ns | 28–50 ns | 100–200 ns | 14–22 M/s |

Projected end-to-end: roughly **3× on insert p50**, **3–4× on insert p99**, and
**4× on a 5-level match**, V0 to V3.

---

## 4. Which optimisation should deliver the largest single win

**V0 → V1, allocation removal**, for `insert` and `cancel`.

V0 performs three allocator operations per insert: the `std::list` node, the
`std::unordered_map` node, and — on a new price level — the `std::map` node.
V1 eliminates the first outright and makes the order record itself free. At
20–35 ns per allocation that is the largest single identifiable line item in the
budget, and unlike the others it also removes a source of variance rather than
just a source of latency.

**For `match` specifically, V1 → V2 should be larger**, because a multi-level
sweep pays the tree cost once per level rather than once per operation. Five
levels means five descents and five potential rebalances in V0/V1, against five
`find_next` calls over a bitmap in L1 for V2.

If the measurements contradict this — if V2 beats V1 by more than V1 beats V0 on
`insert` — the most likely explanation is that glibc's tcache is performing far
better than modelled, which would make the allocation term much smaller than
20–35 ns and correspondingly reduce V1's advantage.

---

## 5. Which optimisation should underdeliver, or regress

### The flat price array (V2), on a sparse or wide book — the primary risk

This is the one to be suspicious of. Three failure modes:

1. **Memory footprint.** `flat_levels = 65,536` across two sides is roughly
   **4 MB of level structures**, allocated whether occupied or not. The Ryzen 7
   260 has ~16 MB of L3 *shared across all cores*. A single book fits; several
   books, or one book alongside anything else on the machine, will evict each
   other. V0's tree allocates only the levels that exist — for a book with 20
   live levels that is under 2 KB.

2. **Cold-level DRAM misses.** A book whose activity moves across the array —
   a trending instrument — touches levels that have not been read for a long
   time. Each is a fresh DRAM access at 53–79 ns, which is more than the entire
   projected V2 insert. Under a trending workload V2 could plausibly land
   *slower* than V1, whose tree keeps only live levels and therefore keeps them
   hot.

3. **Bitmap scan length under sparsity.** The two-level summary bounds the scan
   well, but a book with two orders 40,000 ticks apart still walks more summary
   words than a tree descent would have visited nodes.

**Predicted crossover:** V2 should beat V1 comfortably when active levels number
in the low hundreds and stay clustered. It should be roughly neutral when the
active range spans several thousand ticks. It should lose when the active range
approaches the array size, or when several books share the cache.

`tests/test_v2_boundary.cpp` asserts the *functional* consequence of this — V0
and V1 accept prices V2 must reject — but the performance consequence can only
be measured.

### Branch hints (part of V3) — likely to underdeliver

`MB_LIKELY`/`MB_UNLIKELY` are applied to rejection paths, pool exhaustion and
self-trade collisions. Those branches are genuinely lopsided, so the hints
should not hurt. But Zen's branch predictor is very good at exactly this kind of
strongly-biased branch without help, so the realistic gain is **0–4 ns**, and it
could be zero. If a measured V3-over-V2 improvement is smaller than the compact
`HotOrder` term alone predicts, the hints are the part contributing nothing.

There is also a way for them to make things worse: a hint on a branch that is
*not* actually biased forces the compiler to lay out the wrong path as the
fall-through, costing a taken branch on the common case. If the measured V3 is
slower than V2 on any operation, removing the hints one at a time is the first
thing to try.

### `alignas(64)` — a real cost, not just a benefit

`alignas(64)` on `HotLevel` pads it from what would be 24–32 bytes to a full 64.
For 65,536 levels per side that turns ~4 MB into ~8 MB. It buys guaranteed
single-line access per level and pays for it in cache footprint, which
interacts badly with failure mode 1 above. On a dense book the trade is good; on
a sparse one it compounds the problem.

---

## 6. What would falsify this model

Concrete, checkable predictions. Any of these being wrong means the model is
wrong somewhere specific:

1. **V1's p99 improves proportionally more than its p50.** If not, V0's tail is
   not allocator-driven and something else is producing it.
2. **V2's advantage over V1 is larger on `match` than on `insert`.** If not, the
   tree descent is not the cost the model thinks it is.
3. **V3's `match` improvement exceeds its `insert` improvement.** The compact
   `HotOrder` only pays per maker touched, so it should show up in matching
   first.
4. **Every version's `cancel` p50 is below its `insert` p50.** Cancel does a
   hash lookup and an unlink; insert does that plus a level lookup and a queue
   insertion.
5. **The V0→V3 insert p50 ratio lands between 2× and 4×.** Above 4× and
   something in the V0 path is worse than modelled; below 2× and the
   `unordered_map` cost is dominating everything and the optimisations are
   working on the wrong part of the problem.

Prediction 5 deserves emphasis. In all four versions the order index is a
`std::unordered_map`, which allocates per insert and chases a bucket pointer.
By V3 the model has it as the largest remaining term. **If that is true, the
next optimisation is not more of the same — it is replacing the order index with
an open-addressed table or a dense slot array, which is a V4 this project does
not contain.**

---

## 7. Measurement caveats, stated in advance

- **Laptop frequency scaling.** The Ryzen 7 260 boosts well above 3.8 GHz and
  throttles under sustained load. Absolute nanosecond figures will drift between
  runs and cannot be compared against another machine. **The relative deltas
  between V0–V3 measured in the same run are the trustworthy result.**
- **WSL2.** The TSC is passed through from the Windows host, so `rdtsc` works,
  but the hypervisor may steal time. That shows up as tail outliers rather than
  a shifted median — another reason the harness reports medians across five
  repetitions and an inter-run spread.
- **`perf` is assumed unavailable** under the stock WSL2 kernel, so none of the
  cache-miss or branch-miss claims above can be verified directly. They are
  inferred from the structure of the code, not observed. Confirming them would
  need `perf stat -e cache-misses,branch-misses` on a kernel that supports it.
- **The inter-run spread is the error bar.** If V2 beats V1 by 5% and the runs
  vary by 8%, that comparison is noise. The harness prints the spread next to
  every figure for exactly this reason.

---

## 8. What to do with this file

Delete it. Once `make bench` has run, `bench/results/` holds real numbers and
`bench/report.html` plots them. At that point this document is worse than
useless — it is a set of plausible figures sitting next to a set of real ones,
and the two will eventually get confused.

Replace it with a short note recording where the model was right and where it
was wrong. That note is worth more than either the projections or the
measurements alone.
