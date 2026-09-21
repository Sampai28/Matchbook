# Optimisation log

Measured results for each engine version.

**Machine fingerprint** — every figure below comes from this one configuration:

```
cpu_model        : AMD Ryzen 7 260 w/ Radeon 780M Graphics
hardware_threads : 16
kernel           : Linux 6.6.87.2-microsoft-standard-WSL2
compiler         : g++ 13.3.0
build_type       : Release  (-O2 -DNDEBUG, no -march=native)
tsc_ghz          : 3.820013
timestamp_utc    : 2026-09-21T01:28:55Z
```

Run natively under WSL2, pinned with `taskset -c 2`, 200,000 timed operations
per repetition, 20,000 discarded as warmup, median of 5 independent
repetitions. Raw output is in `bench/results/`.

Laptop frequency scaling means absolute figures drift between sessions. The
relative deltas between versions in the same run are the trustworthy result,
and the inter-run spread column is the error bar that says whether a difference
means anything.

---

## V0 — naive baseline

**Hypothesis (written before measuring):**
> Dominated by three allocator round trips per insert and a red-black tree
> descent of ~7 nodes with poor locality. Projected insert p50 110–180 ns,
> insert p99 300–600 ns, and the widest p99/p50 ratio of the four.

**Change:** none — this is the baseline.

**Measured result:**

| Metric | Value | Inter-run spread |
|---|---|---|
| insert p50 | 69 ns | 0% |
| insert p99 | 119 ns | — |
| insert p99.9 | 298 ns | — |
| cancel p50 | 119 ns | 0% |
| match p50 (5 levels) | 39 ns | 25.6% |
| throughput (insert) | 8,175,231 ops/s | 55.2% |

**p99 / p50 ratio:** 1.72

**Verdict — the projection was wrong, and informatively so.**

Insert p50 came in at 69 ns against a projected 110–180 ns, and the p99/p50
ratio was 1.72 against a predicted "widest of the four". The cost model assumed
three trips to a general-purpose allocator; in practice glibc's per-thread
tcache serves a repeated same-size allocation from a free list without touching
a lock or the arena, so the allocation cost is far closer to a pointer bump
than the model allowed.

That matters beyond V0: it predicts V1's win will be **smaller** than projected,
because the thing V1 removes was already cheap.

---

## V1 — allocation removal (pool + intrusive lists)

**Hypothesis:**
> Should take 20–35 ns off insert p50 and remove an entire long-tail failure
> mode. **The p99 improvement should exceed the p50 improvement
> proportionally.** Predicted to be the largest single win of the series for
> `insert` and `cancel`.

**Change:** fixed-capacity `OrderPool` with an intrusive free list;
`IntrusiveList` price levels; order index maps to `Order*`; level aggregate
maintained incrementally.

**Measured result:**

| Metric | Value | Δ vs V0 | Inter-run spread |
|---|---|---|---|
| insert p50 | 59 ns | **−10 ns (−14%)** | 0% |
| insert p99 | 79 ns | −40 ns (−34%) | — |
| insert p99.9 | 268 ns | −30 ns (−10%) | — |
| cancel p50 | 109 ns | −10 ns (−8%) | 9.2% |
| match p50 | 29 ns | −10 ns (−26%) | 0% |
| throughput (insert) | 11,436,917 ops/s | +40% | 15.2% |

**Did p99 improve proportionally more than p50?** **Yes** — p99 fell 34% against
p50's 14%, which is the one part of the V0/V1 prediction that held exactly.

**Verdict.** Directionally right, magnitude wrong. The −10 ns on p50 is at the
bottom of the projected 20–35 ns range, for the reason V0 exposed: tcache had
already made the allocation cheap. But the tail behaved as predicted, and that
is the more important half — removing allocation removes a *variance* source,
not just a cost, and the p99 improvement being 2.4× the p50 improvement is
exactly the signature of a jitter source being eliminated rather than a
constant being reduced.

Not the largest single win for `cancel` as predicted; V2 took that.

---

## V2 — cache-conscious price levels (flat array + bitmap)

**Hypothesis:**
> Should remove ~46 ns from every operation that locates a price level.
> Projected insert p50 45–85 ns, match p50 140–260 ns. **The advantage over V1
> should be larger on `match` than on `insert`.** Also the optimisation most
> likely to underdeliver or regress.

**Change:** `std::vector<IntrusiveList>` indexed by ticks-from-base per side;
two-level occupancy bitmap using `countr_zero`/`countl_zero`; prices outside the
window rejected with `PriceOutsideArray`.

**Measured result:**

| Metric | Value | Δ vs V1 | Inter-run spread |
|---|---|---|---|
| insert p50 | 49 ns | −10 ns (−17%) | 0% |
| insert p99 | **99 ns** | **+20 ns (+25%) — regression** | — |
| insert p99.9 | 228 ns | −40 ns (−15%) | — |
| cancel p50 | **39 ns** | **−70 ns (−64%)** | 0% |
| match p50 | 29 ns | 0 ns (no change) | 0% |
| throughput (insert) | 12,283,009 ops/s | +7% | 12.4% |

**Was the `match` gain larger than the `insert` gain?** **No.** Insert improved
17%; match did not move at all. The prediction was backwards.

### The insert p99 regression

This is the most interesting single number in the engine results. V2's median
insert improved while its 99th percentile got **25% worse**. The flat array
replaces a predictable pointer chase through a small hot tree with an indexed
load into a 4 MB array — which is fast when the line is resident and a DRAM
round trip when it is not. The median case hits a warm line and wins; the tail
case walks into a cold one and loses more than the tree descent ever cost.

That is precisely the risk the hypothesis flagged, appearing in the tail rather
than the median. A summary that reported only p50 would have recorded this pass
as a clean win.

### Sparse-book check — the prediction was backwards

Re-run with `--depth 2000` to spread activity across the array. V2 was predicted
to lose here, since a wider active range means more cold levels.

| Metric | V1 (sparse) | V2 (sparse) | Winner |
|---|---|---|---|
| insert p50 | 101 ns | **60 ns** | V2 by 41% |
| insert p99 | 424 ns | **272 ns** | V2 by 36% |
| cancel p50 | 202 ns | **181 ns** | V2 by 10% |
| match p50 | 30 ns | 30 ns | tie |
| insert throughput | 5,619,973 ops/s | **9,284,301 ops/s** | V2 by 65% |

**V2 won the configuration it was predicted to lose, and won it decisively.**

The model reasoned about V2's cost growing as the book spreads out. It ignored
that V1's cost grows too, and faster: a `std::map` over 2,000 live price levels
is a deeper tree with worse locality at every level, and its insert p50 nearly
doubled (59 → 101 ns) while V2's rose by only 22% (49 → 60 ns). The flat array's
cost is bounded by one cache miss; the tree's is bounded by log₂(n) of them.

So the crossover the hypothesis expected to find between depth 20 and depth
2000 is not there. Wherever it is, it is deeper than this book gets — which
makes the flat array a better default than the design notes assumed.

**Verdict.** Underdelivered on the stated axis (match), overdelivered on one
nobody predicted (cancel, −64%), regressed on insert p99, and won the sparse
configuration it was supposed to lose. Four predictions, four different kinds
of wrong.

---

## V3 — hot-path tuning

**Hypothesis:**
> Projected insert p50 32–65 ns, match p50 100–200 ns. **The `match`
> improvement should exceed the `insert` improvement**, because the compact
> record only pays off per maker touched. Branch hints expected to underdeliver.

**Change:** packed `HotOrder`; `alignas(64)` `HotLevel`; cached
`best_bid_idx_`/`best_ask_idx_`; `tick_shift_` fast path for power-of-two ticks;
`MB_LIKELY`/`MB_UNLIKELY` on rejection paths.

**Measured result:**

| Metric | Value | Δ vs V2 | Δ vs V0 | Inter-run spread |
|---|---|---|---|---|
| insert p50 | 39 ns | −10 ns (−20%) | **−30 ns (1.77×)** | 25.6% |
| insert p99 | 79 ns | −20 ns (−20%) | −40 ns (1.51×) | — |
| insert p99.9 | **129 ns** | −99 ns (−43%) | **−169 ns (2.31×)** | — |
| cancel p50 | 39 ns | 0 ns | −80 ns (3.05×) | 179% ⚠ |
| match p50 | 29 ns | 0 ns | −10 ns (1.34×) | 0% |
| throughput (insert) | 13,674,164 ops/s | +11% | +67% | 7.8% |

**Was the `match` gain larger than the `insert` gain?** **No.** Match did not
move. Insert improved 20%. Wrong for the second version running.

⚠ V3's cancel p50 carries a 179% inter-run spread. That figure is not
trustworthy and no conclusion should be drawn from it; the run-to-run variation
exceeds the quantity being measured. The other V3 rows have spreads under 26%.

**Verdict.** The clearest win is where nobody looked: **insert p99.9 fell 43%**,
from 228 ns to 129 ns, and is 2.31× better than V0 — a larger improvement than
the median saw at any step. V3's changes are mostly about touching fewer cache
lines, and that shows up in the tail, where a cache miss decides the outcome,
rather than in the median, where it does not.

The ablation table below is unfilled: isolating four changes requires four
additional builds with individual `#ifdef`s that the source does not currently
carry, and adding them is a code change rather than a measurement. The
aggregate V2→V3 delta is real; the attribution between the four changes is not
established.

| Change removed | insert p50 | match p50 | Kept? |
|---|---|---|---|
| Branch hints | | | |
| Cached top-of-book | | | |
| Tick shift fast path | | | |
| Compact `HotOrder` | | | |

---

## Overall

| Version | insert p50 | insert p99 | insert p99.9 | cancel p50 | match p50 | throughput |
|---|---:|---:|---:|---:|---:|---:|
| V0 | 69 ns | 119 ns | 298 ns | 119 ns | 39 ns | 8.18 M/s |
| V1 | 59 ns | 79 ns | 268 ns | 109 ns | 29 ns | 11.44 M/s |
| V2 | 49 ns | 99 ns | 228 ns | 39 ns | 29 ns | 12.28 M/s |
| V3 | 39 ns | 79 ns | 129 ns | 39 ns | 29 ns | 13.67 M/s |

**End-to-end V0 → V3:** **1.77×** on insert p50, **1.51×** on insert p99,
**2.31×** on insert p99.9, **3.05×** on cancel p50, **1.34×** on match p50.
Insert throughput rose 67%.

**Projected was 3× / 3–4× / 4×.** Every headline projection overshot. The
one place reality beat the projection is insert p99.9, which nobody projected
at all.

### Where the cost model was wrong

| Prediction | Outcome | What the model missed |
|---|---|---|
| V1's p99 improves proportionally more than p50 | ✅ **Correct** — 34% vs 14% | Nothing. The one clean hit. |
| V2's gain is larger on match than insert | ❌ **Backwards** — insert −17%, match 0% | Match at this depth is already dominated by the fill loop, not by level lookup. Locating the level was never the expensive part of matching. |
| V3's gain is larger on match than insert | ❌ **Backwards** — insert −20%, match 0% | Same cause. Both versions optimised level *location* and measured it against an operation that barely does any. |
| Every version's cancel p50 < insert p50 | ❌ **False for V0 and V1** — cancel 119/109 ns vs insert 69/59 ns | The model assumed cancel is "insert minus the matching work". It is not: cancel pays an `unordered_map` lookup to find the order, then a level-aggregate update, and in V0/V1 it also pays a tree descent to reach the level. Insert amortises its descent against work already in cache. |
| V0→V3 insert p50 ratio between 2× and 4× | ❌ **1.77×, below the range** | The baseline was faster than projected (tcache), so there was less to remove. Starting from a wrong baseline makes every downstream ratio wrong. |
| V2 loses on a sparse book | ❌ **V2 won by 41% on insert p50** | Modelled V2's degradation with depth, ignored V1's. `std::map` degrades faster. |

Five of six predictions wrong, in four distinct ways. The one that held —
allocation removal helping the tail more than the median — is also the one with
a mechanism behind it rather than an arithmetic estimate.

### What the next optimisation should be

The model predicted the `std::unordered_map` order index would be the dominant
remaining term by V3. The measurements support that indirectly rather than
directly: V3's cancel p50 sits at 39 ns and did not improve over V2 despite V3
touching fewer cache lines, and cancel is the operation most dominated by the
index lookup. Insert p50 at 39 ns is also within range of a single hash lookup
plus a miss.

**Remaining bottleneck after V3:** most likely the order index, on the evidence
that both remaining operations plateaued at the same 39 ns regardless of what
else changed. Not proven — proving it needs either the ablation builds above or
a profiler, and `perf` is unavailable under this kernel.

**Proposed V4:** replace `std::unordered_map<ClientOrderId, Order*>` with an
open-addressed table with linear probing, or a dense slot array keyed on a
dense internal id. Either removes the per-insert node allocation and the bucket
pointer chase. Expected to show up on cancel first, since cancel is the
operation with nothing else left in it.

A secondary candidate, which the results argue for more strongly than the model
did: **the insert p99 regression V2 introduced and V3 only partly recovered**.
V2's p99 is 99 ns against V1's 79 ns, and V3 got back to 79 ns without beating
it. Prefetching the target level, or a smaller level record, would target the
cold-miss tail directly.
