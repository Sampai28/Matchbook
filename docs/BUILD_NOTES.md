# Build notes

**Nothing in this repository has been compiled, run, or tested.** Every file was
written to be correct on inspection. This document is the honest list of where
that is most likely to have failed, written for someone who has not built a C++
project before.

Expect the first build to fail. That is normal for a project of this size that
has never seen a compiler, and it is not a sign that something is deeply wrong.
Most first-build errors are one missing `#include` or one signature mismatch.

---

## 1. The exact first-build sequence

Run these in order, in WSL2 Ubuntu, from the repository root. Do not skip to
step 4.

### Step 1 — check the toolchain

```bash
g++ --version && cmake --version && ninja --version && python3 --version
```

**Success:** g++ 13.x, CMake 3.22 or newer, Ninja present, Python 3.9+.

**Most likely failure:** one of them is missing.

```bash
sudo apt-get update && sudo apt-get install -y build-essential cmake ninja-build git python3
```

If g++ is older than 13, the C++20 features used here — `std::countr_zero`,
`std::countl_zero` from `<bit>`, designated initialisers, `[[likely]]` — may not
all be available. g++ 11 will probably work; g++ 9 will not.

### Step 2 — configure

```bash
cmake --preset release
```

This is the step that reaches the network, to fetch Catch2 and cpp-httplib. It
takes a minute or two the first time and is cached afterwards.

**Success:** ends with the `matchbook 0.1.0` summary block and no errors.

**Most likely failures:**

- *`CMake Error: Could not find CMAKE_MAKE_PROGRAM`* — Ninja is not installed.
  Install it, or add `-G "Unix Makefiles"` to fall back to make.
- *FetchContent fails to clone* — no network, or a proxy. Both dependencies can
  be disabled: `cmake --preset release -DMATCHBOOK_BUILD_TESTS=OFF
  -DMATCHBOOK_BUILD_SERVER=OFF` builds the engines, the replay tool and the
  benchmark with no external dependencies at all.
- *Catch2 v3.5.2 tag not found* — the tag was chosen from memory and not
  verified. Check <https://github.com/catchorg/Catch2/tags> and substitute a
  current v3 tag in `CMakeLists.txt`. Same for cpp-httplib v0.15.3.

### Step 3 — compile just the core library first

```bash
cmake --build --preset release --target matchbook_core
```

Building the smallest thing first is deliberate: it isolates engine compile
errors from test and server errors, so the first wall of output is about one
thing.

**Success:** `matchbook_core` links, no errors.

**Most likely failures:** see §2 below — this is where they will be.

### Step 4 — compile everything

```bash
cmake --build --preset release -j
```

### Step 5 — run the unit tests

```bash
ctest --preset release --output-on-failure
```

**Success:** all test cases pass.

**Most likely failure:** a genuine logic bug in one engine that the differential
tests inside `test_price_time.cpp` and `test_cancel_replace.cpp` catch as a log
mismatch between V0 and V1/V2/V3. The failure output prints both logs. V0 is the
reference; if V0 disagrees with the others, suspect V0 last.

### Step 6 — replay determinism

```bash
make replay-diff
```

**Success:** "Replay is byte-identical across runs and across all four engines."

### Step 7 — the differential oracle against Python

```bash
make diff-test
```

This is the strongest correctness check in the project: an independent Python
implementation against all four C++ engines over 50,000 operations.

**Success:** "All four engines match the Python reference on events and final
state."

**Most likely failure:** a semantic divergence between
`tools/reference_matcher.py` and the C++ engines. `tools/validate_fills.py`
prints the first differing event with context. See §4 for the specific places
these two implementations are most likely to disagree.

### Step 8 — only now, benchmark

```bash
./bench/run_bench.sh
```

Natively, not in Docker. See §6.

---

## 2. Where the first build will most likely break

Ranked by how likely I think each is.

### 2.1 Missing standard-library includes — very likely

The most common failure in never-compiled C++. A file uses `std::sort` but only
transitively included `<algorithm>` through another header, and the transitive
include differs between standard library versions.

Known risk spots:

| File | Uses | Include present? |
|---|---|---|
| `include/matchbook/metrics.hpp` | `std::fill` | `<algorithm>` added |
| `src/engine/common/bitmap.hpp` | `std::fill`, `std::countr_zero` | `<algorithm>`, `<bit>` added |
| `bench/hdr_histogram.hpp` | `std::clamp`, `std::snprintf` | `<algorithm>` present; **`<cstdio>` may be missing** |
| `bench/machine_fingerprint.hpp` | `std::time`, `std::strftime`, `std::gmtime` | **`<ctime>` is probably missing** |
| `src/server/json.hpp` | `std::snprintf` | **`<cstdio>` may be missing** |
| `tests/test_validation.cpp` | `std::sort`, `std::adjacent_find` | **`<algorithm>` is probably missing** |

The fix is always the same: add the include named in the error. Do not
restructure anything.

### 2.2 `-Wconversion` warnings — very likely, and they are warnings not errors

The build enables `-Wconversion -Wsign-conversion`, which is stricter than most
projects use. There will be narrowing warnings, particularly around
`std::size_t` ↔ `std::int32_t` in the level-index arithmetic in
`v2_book.cpp` and `v3_book.cpp`.

They will not stop the build. Fix them with explicit `static_cast` rather than
by disabling the warning; each one is a place where a negative index or an
overflow could hide.

If the noise is overwhelming and you want to make progress first, comment out
`-Wconversion -Wsign-conversion` in `CMakeLists.txt` and put them back later.

### 2.3 `v3_book.cpp` — `MB_LIKELY` inside a `constexpr` context

`MB_UNLIKELY` is used inside `price_to_index`, which is a `const` member
function. `__builtin_expect` is fine there. But if you later mark that function
`constexpr`, `__builtin_expect` becomes a constant-expression problem on some
compiler versions. It is not `constexpr` now; do not make it so.

### 2.4 `HotOrder` field-offset comments may be wrong

The comments in `v3_book.cpp` claim specific byte offsets (`next` at 0,
`remaining` at 8, and so on). The compiler's actual layout depends on alignment
rules I did not verify. **The comments could be wrong while the code is
correct.** To check:

```cpp
static_assert(offsetof(HotOrder, remaining) == 8, "layout changed");
```

Add those temporarily, see what the compiler says, and correct the comments.
Nothing depends on the exact offsets — only the *grouping* of hot fields
matters — so a wrong comment is a documentation bug, not a code bug.

### 2.5 `Engine::submit` copies `Stats` per event

`src/engine/engine.cpp` does `Stats dummy = entry->book->stats();` inside a loop
over events. `Stats` contains a `std::array` of 17 counters, so that copies
~200 bytes per event. It is correct but wasteful, and it means the invariant
checker's `stats.invariant_violations++` writes to a temporary and is
**discarded**.

**This is a real bug, not a style issue.** The violation counter increments on a
copy and the increment is lost. The `/stats` endpoint reads violations from
`InvariantChecker::violations()` instead, which is why it is not visible in the
output — but the `Stats::invariant_violations` field will always read zero.

Fix: give `IBook` a `Stats& mutable_stats()` accessor, or move the counter
entirely into `InvariantChecker` and delete the field. I did not do it because I
could not test the change.

### 2.6 cpp-httplib target name

`CMakeLists.txt` links `httplib::httplib`. Depending on the version, the
exported target may be plain `httplib`. If the link fails with "target not
found", try:

```cmake
target_link_libraries(matchbook-server PRIVATE matchbook_core httplib)
```

cpp-httplib is header-only, so a third fallback is to drop the link entirely and
add `${httplib_SOURCE_DIR}` to the include path.

### 2.7 Catch2 CTest integration

`catch_discover_tests` needs `${catch2_SOURCE_DIR}/extras` on the module path.
The variable is spelled `catch2_SOURCE_DIR` (lowercase) by FetchContent even
though the package is declared as `Catch2`. If `include(Catch)` fails, print the
variable to check:

```cmake
message(STATUS "catch2 src: ${catch2_SOURCE_DIR}")
```

Worst case, delete the `catch_discover_tests` call and register the binary as
one test:

```cmake
add_test(NAME all COMMAND matchbook-tests)
```

### 2.8 `bench_main.cpp` uses `w.passive[idx].participant` in `measure_cancel`

The cancel loop indexes `w.passive[idx]` where `idx` walks the `resting` vector.
Those indices only line up if every passive order rested, which is not
guaranteed — some cross and fill immediately. **The participant may not match,
in which case the cancel is rejected and the benchmark measures the rejection
path rather than the cancel path.**

Fix: store the participant alongside the client id when building `resting`:

```cpp
std::vector<std::pair<ClientOrderId, ParticipantId>> resting;
```

I noticed this while writing it and left it because changing it without being
able to run the result risked making it worse. **Fix this before trusting any
cancel number.**

### 2.9 `HdrHistogram::counts_index` bucket-0 handling

The index arithmetic in `bench/hdr_histogram.hpp` is a from-memory
reconstruction of the HdrHistogram algorithm. The bucket-0 special case and
`value_from_index` are the parts I am least sure of; an off-by-one there would
shift reported percentiles without producing any visible error.

**How to check it before trusting a single number:**

```cpp
HdrHistogram h;
for (int i = 1; i <= 100000; ++i) h.record(i);
// p50 should be ~50000, p99 ~99000, max 100000
```

If those come out wrong, configure with `-DMATCHBOOK_USE_HDR_LIB=ON` to use the
real library instead. That path is wired up but also untested.

---

## 3. Things I could not verify at all

- **Whether any of it compiles.** No compiler was run.
- **Whether the four engines actually agree.** The differential test exists but
  has never executed. V1, V2 and V3 were each written by transcribing V0's
  matching rules; a transcription error is entirely possible and is precisely
  what `make diff-test` is for.
- **Whether `web/app.js` renders correctly.** No browser was opened.
- **Whether the Docker build works.** No image was built, no container started.
- **Whether the Catch2 and cpp-httplib tags exist.** Both were chosen from
  memory.
- **Whether `taskset -c 2` is available under WSL2.** It usually is, via
  `util-linux`, but the harness prints a warning and continues without it.
- **Every number in `docs/expected-performance.md`.** All projections.

---

## 4. Where the Python oracle and the C++ engines are most likely to disagree

`make diff-test` compares two independent implementations. The most likely
sources of a false alarm — a disagreement that is a specification ambiguity
rather than an engine bug:

1. **Sequence number consumption.** Both implementations increment a counter for
   the taker order before matching (`self.seq += 1` in Python,
   `taker.seq = ++seq_` in C++). If one path skips that increment — the
   POST_ONLY rejection path is the risky one — every subsequent sequence number
   differs and the whole log diverges from that point.

2. **Event ordering around self-trade prevention.** Both emit a `CANCEL` for the
   resting order and then continue matching. If one emits the cancel *after* the
   next fill, the logs diverge with identical content in a different order.

3. **The replace "keeps priority" condition.** C++ uses
   `req.new_quantity > already && req.new_quantity <= o->quantity`; Python uses
   `o.filled < new_qty <= o.quantity`. Those should be the same predicate.
   Confirm they are.

4. **Rejection precedence.** The C++ engine validates before checking for a
   duplicate client order id (validation is in `Engine::submit`, the duplicate
   check is in the book). Python's `Book.submit` validates first too. If an
   order is both invalid *and* a duplicate, both must report the same reason.

5. **`kNoPrice` vs `None`.** C++ uses `INT64_MIN` as the sentinel; Python uses
   `None`. Any place C++ does arithmetic on `kNoPrice` before checking it is a
   latent overflow and a divergence.

When the diff fails, read `tools/validate_fills.py` output first: it names the
first divergent event and prints ten lines of context either side. The bug is
almost always in the operation immediately before the first differing line.

---

## 5. Recommended first-run order, restated

Do not run `make bench` first. It is the most satisfying target and the least
informative one until correctness is established.

```
1. cmake --preset release          # configure
2. cmake --build --preset release --target matchbook_core
3. cmake --build --preset release -j
4. ctest --preset release --output-on-failure
5. make replay-diff                # determinism
6. make diff-test                  # the real correctness check
7. make fuzz                       # robustness
8. ./bench/run_bench.sh            # only now
9. python3 tools/make_report.py
```

Steps 1–3 are about the compiler. Steps 4–7 are about correctness. Only step 8
produces a number worth quoting, and only if 4–7 passed.

---

## 6. Docker versus native, and why the split exists

**Build and test in Docker.** `make docker-test` runs the unit tests, the replay
determinism check, the differential oracle and a short fuzz run in a
reproducible environment with a pinned `gcc:13` — matching WSL's g++ 13.3.0, so
the same code generation is exercised in both places.

**Benchmark natively in WSL2.** `make bench` refuses to run inside a container
(it checks for `/.dockerenv`). Containerised timing on a Windows host inherits
scheduling jitter from Hyper-V and I/O latency from the filesystem shim. At
millisecond resolution that is invisible. At the tens-of-nanoseconds resolution
this harness works in, it is the dominant term, and the numbers would describe
the virtualisation stack rather than the order book.

`perf` is assumed unavailable under the stock WSL2 kernel, so the harness relies
entirely on in-process timing: `rdtsc` for per-operation latency and
`steady_clock` for wall time. That means cache-miss and branch-miss counts
cannot be confirmed — the claims about them in
`docs/expected-performance.md` are inferred from code structure, not observed.

---

## 7. If you only fix three things before benchmarking

1. **`measure_cancel`'s participant mismatch** (§2.8) — otherwise the cancel
   numbers measure the rejection path.
2. **The HdrHistogram bucket arithmetic** (§2.9) — otherwise every percentile
   could be silently shifted.
3. **The discarded `invariant_violations` counter** (§2.5) — otherwise a
   corruption in release mode is invisible in `/stats`.

None of these will stop the build. All three would quietly produce wrong output.
