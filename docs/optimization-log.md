# Optimisation log

**This is a template. Every result field is blank because nothing has been
measured.** Fill it in as each version is benchmarked. The projections in
`docs/expected-performance.md` are the hypotheses being tested; this file records
what actually happened.

Keep the hypotheses honest: write them down *before* running the benchmark, not
after seeing the number. An optimisation log where every hypothesis turned out
correct is a log that was written backwards.

---

## How to use this file

For each version, after running `./bench/run_bench.sh`:

1. Copy the measured p50/p99/p99.9 and throughput from the newest file in
   `bench/results/`.
2. Fill in the *measured result* row.
3. Write the *verdict* honestly. "No measurable change" and "slower" are results.
4. If the verdict contradicts the projection, say what the cost model got wrong.

---

## V0 — naive baseline

**Hypothesis (before measuring):**
> `std::map` + `std::list` + `std::unordered_map`, allocating freely. This is
> the correctness reference, not a performance candidate. Expected to be
> dominated by three allocator round trips per insert and a red-black tree
> descent of ~7 nodes with poor locality. Projected insert p50 110–180 ns,
> insert p99 300–600 ns, and the widest p99/p50 ratio of the four.

**Change:** none — this is the baseline.

**Measured result:**

| Metric | Value | Inter-run spread |
|---|---|---|
| insert p50 | | |
| insert p99 | | |
| insert p99.9 | | |
| cancel p50 | | |
| match p50 (5 levels) | | |
| throughput | | |

**p99 / p50 ratio:** _____

**Verdict:**

_(Was the tail as wide as predicted? If not, glibc's tcache is doing better than
the model assumed, which changes the expected size of V1's win.)_

---

## V1 — allocation removal (pool + intrusive lists)

**Hypothesis (before measuring):**
> Removing the `std::list` node allocation and pooling `Order` records should
> take 20–35 ns off insert p50 and, more importantly, remove an entire long-tail
> failure mode. **The p99 improvement should exceed the p50 improvement
> proportionally.** The `std::map` ladder is untouched, so the tree descent
> (~46 ns including mispredicts) becomes the dominant remaining term. Projected
> insert p50 75–125 ns, insert p99 180–350 ns.
>
> Predicted to be the largest single win of the series for `insert` and `cancel`.

**Change:**
- `detail::OrderPool` — fixed-capacity pool, intrusive free list threaded
  through `Order::next`. Never grows: reallocation would invalidate every
  intrusive pointer in the book.
- `detail::IntrusiveList` — price levels hold orders by embedded prev/next
  links rather than in a `std::list`.
- Order index maps to `Order*` rather than to a list iterator.
- Level aggregate quantity maintained incrementally.

**Measured result:**

| Metric | Value | Δ vs V0 | Inter-run spread |
|---|---|---|---|
| insert p50 | | | |
| insert p99 | | | |
| insert p99.9 | | | |
| cancel p50 | | | |
| match p50 (5 levels) | | | |
| throughput | | | |

**Did p99 improve proportionally more than p50?** _____

**Verdict:**

---

## V2 — cache-conscious price levels (flat array + bitmap)

**Hypothesis (before measuring):**
> Replacing the tree descent with `(price - base) / tick` and one indexed load
> should remove ~46 ns from every operation that locates a price level. A
> two-level occupancy bitmap keeps best-bid/ask at 3–6 ns. Projected insert p50
> 45–85 ns, match p50 140–260 ns.
>
> **The advantage over V1 should be larger on `match` than on `insert`**, because
> a 5-level sweep pays the tree cost five times but the index cost five times
> more cheaply.
>
> **This is also the optimisation most likely to underdeliver or regress.** The
> flat array costs ~4 MB per book regardless of occupancy against ~16 MB of
> shared L3, cannot represent prices outside its window at all, and takes a
> 53–79 ns DRAM miss on every cold level a trending book walks into.

**Change:**
- `std::vector<IntrusiveList>` indexed by ticks-from-base, per side.
- `detail::LevelBitmap` — two-level occupancy bitmap; `find_first`/`find_last`/
  `find_next`/`find_prev` via `countr_zero`/`countl_zero`.
- Prices outside the window rejected with `PriceOutsideArray` rather than
  clamped or wrapped.

**Measured result:**

| Metric | Value | Δ vs V1 | Inter-run spread |
|---|---|---|---|
| insert p50 | | | |
| insert p99 | | | |
| insert p99.9 | | | |
| cancel p50 | | | |
| match p50 (5 levels) | | | |
| throughput | | | |

**Was the `match` gain larger than the `insert` gain?** _____

**Sparse-book check** — rerun with `--depth 2000` to spread activity across the
array, and record whether V2 still beats V1:

| Metric | V1 (sparse) | V2 (sparse) | Winner |
|---|---|---|---|
| insert p50 | | | |
| match p50 | | | |

**Verdict:**

_(If V2 loses on the sparse configuration, that is the predicted result, not a
failure. Record where the crossover falls — it is the most interesting number in
this whole project.)_

---

## V3 — hot-path tuning

**Hypothesis (before measuring):**
> Four changes, with individually predicted effects:
>
> | Change | Predicted |
> |---|---|
> | Power-of-two tick → shift instead of 64-bit divide | −5 to −10 ns per insert |
> | Cached best-bid/ask index | −3 to −6 ns per submit |
> | Compact `HotOrder`, 1 cache line per maker instead of 2 | −3.7 to −11 ns **per maker touched** |
> | Branch hints on validated fast paths | −0 to −4 ns, possibly zero |
>
> Projected insert p50 32–65 ns, match p50 100–200 ns.
>
> **The `match` improvement should exceed the `insert` improvement**, because the
> compact record only pays off per maker touched.
>
> The branch hints are expected to underdeliver: Zen predicts strongly-biased
> branches well without help.

**Change:**
- `HotOrder` — hot fields (`next`, `remaining`, `price`, `participant`) packed
  into the first 32 bytes.
- `HotLevel` — `alignas(64)`, aggregate adjacent to the head pointer.
- Cached `best_bid_idx_` / `best_ask_idx_`, rescanned only when the top level
  empties.
- `tick_shift_` fast path when the tick size is a power of two.
- `MB_LIKELY` / `MB_UNLIKELY` on rejection, pool-exhaustion and self-trade paths.

**Measured result:**

| Metric | Value | Δ vs V2 | Δ vs V0 | Inter-run spread |
|---|---|---|---|---|
| insert p50 | | | | |
| insert p99 | | | | |
| insert p99.9 | | | | |
| cancel p50 | | | | |
| match p50 (5 levels) | | | | |
| throughput | | | | |

**Was the `match` gain larger than the `insert` gain?** _____

**Ablation — which of the four changes actually paid?**

Rebuild V3 with each change disabled in turn and record the delta. Any change
whose removal costs less than the inter-run spread contributed nothing
measurable and should be reverted for the simplicity.

| Change removed | insert p50 | match p50 | Kept? |
|---|---|---|---|
| Branch hints | | | |
| Cached top-of-book | | | |
| Tick shift fast path | | | |
| Compact `HotOrder` | | | |

**Verdict:**

---

## Overall

| Version | insert p50 | insert p99 | cancel p50 | match p50 | throughput |
|---|---|---|---|---|---|
| V0 | | | | | |
| V1 | | | | | |
| V2 | | | | | |
| V3 | | | | | |

**End-to-end V0 → V3:** _____× on insert p50, _____× on insert p99,
_____× on match p50.

**Projected was 3× / 3–4× / 4×.**

### Where the cost model was wrong

_(This is the most valuable section in the file. Fill it in properly.)_

| Prediction | Outcome | What the model missed |
|---|---|---|
| V1's p99 improves proportionally more than p50 | | |
| V2's gain is larger on match than insert | | |
| V3's gain is larger on match than insert | | |
| Every version's cancel p50 < insert p50 | | |
| V0→V3 insert p50 ratio between 2× and 4× | | |

### What the next optimisation should be

_(The model predicts that by V3 the `std::unordered_map` order index is the
largest remaining term, since it allocates per insert and chases a bucket
pointer in every version. If the measurements agree, replacing it with an
open-addressed table or a dense slot array is V4. If they disagree, write down
what the real bottleneck turned out to be.)_

Remaining bottleneck after V3: _____

Proposed V4: _____
