// matchbook-bench — the latency and throughput harness.
//
// NOTHING IN THIS FILE HAS BEEN RUN. It is written to be correct on inspection
// and is unverified. See docs/BUILD_NOTES.md.
//
// Methodology, and why each piece is there:
//
//   * Pin to one core (taskset -c 2, applied by run_bench.sh, not here). A
//     migration mid-run invalidates every cache line the book occupies and
//     shows up as a step change in the tail.
//
//   * Discard warmup iterations. The first few thousand operations populate the
//     branch predictors, the TLB, and the allocator's arenas. Including them
//     measures start-up, not steady state.
//
//   * At least 5 independent repetitions, reporting the median and the spread.
//     A single run on a laptop is a sample of one from a noisy distribution.
//     The inter-run spread is reported because it is the honest error bar: if
//     V2 beats V1 by 3% and runs vary by 8%, the comparison means nothing.
//
//   * rdtsc for per-operation timing (~6-10 cycles) and steady_clock for wall
//     time. Timing a 40ns operation with a 20ns clock puts half the measurement
//     in the instrument.
//
//   * The workload is generated once and replayed identically for every
//     version, so the comparison is not confounded by different input.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "bench/hdr_histogram.hpp"
#include "bench/machine_fingerprint.hpp"
#include "bench/rdtsc.hpp"
#include "matchbook/book.hpp"
#include "matchbook/engine.hpp"

namespace {

using namespace matchbook;
using namespace matchbook::bench;

struct Options {
    std::size_t iterations = 200'000;
    std::size_t warmup     = 20'000;
    std::size_t reps       = 5;
    std::size_t book_depth = 20;      // levels pre-populated per side
    std::size_t match_levels = 5;     // levels a crossing order should sweep
    std::string out_dir    = "bench/results";
    std::uint64_t seed     = 0xC0FFEE;
    std::vector<EngineVersion> versions = {
        EngineVersion::V0_Naive, EngineVersion::V1_Pool,
        EngineVersion::V2_Flat,  EngineVersion::V3_Tuned,
    };
};

BookConfig bench_config() {
    BookConfig cfg;
    cfg.symbol          = "BENCH";
    cfg.tick_size       = 1;
    cfg.reference_price = 100'000;
    cfg.price_band_ticks = 50'000;
    cfg.max_orders      = 1u << 21;
    cfg.flat_levels     = 1u << 16;
    // Invariant checking is disabled for benchmarking. It is O(orders) and
    // would dominate every measurement; correctness is established by the test
    // suite and the differential oracle, not here.
    cfg.invariant_mode  = InvariantMode::Off;
    return cfg;
}

// One pre-generated workload, replayed identically against every version.
struct Workload {
    std::vector<NewOrder> passive;   // resting orders that build the book
    std::vector<NewOrder> crossing;  // aggressors that sweep N levels
    std::vector<ClientOrderId> cancel_targets;
};

Workload generate(const Options& opt) {
    Workload w;
    std::mt19937_64 rng(opt.seed);

    const Price ref = 100'000;
    ClientOrderId cid = 1;

    // Passive orders spread across `book_depth` levels on each side, clustered
    // near the touch the way real flow is.
    const std::size_t passive_count = opt.iterations + opt.warmup;
    w.passive.reserve(passive_count);
    for (std::size_t i = 0; i < passive_count; ++i) {
        NewOrder o;
        o.client_id   = cid++;
        o.participant = static_cast<ParticipantId>(1 + (rng() % 8));
        o.side        = (rng() & 1) ? Side::Buy : Side::Sell;
        o.type        = OrdType::Limit;
        const auto offset = static_cast<Price>(1 + (rng() % opt.book_depth));
        // Passive orders never cross: bids below the reference, asks above.
        o.price    = (o.side == Side::Buy) ? ref - offset : ref + offset;
        o.quantity = static_cast<Qty>(10 + (rng() % 90));
        w.passive.push_back(o);
    }

    // Crossing orders priced deep enough through the book to sweep
    // `match_levels` price levels.
    const std::size_t crossing_count = opt.iterations + opt.warmup;
    w.crossing.reserve(crossing_count);
    for (std::size_t i = 0; i < crossing_count; ++i) {
        NewOrder o;
        o.client_id   = cid++;
        o.participant = 99;  // never collides with passive participants 1-8
        o.side        = (rng() & 1) ? Side::Buy : Side::Sell;
        o.type        = OrdType::IOC;
        const auto reach = static_cast<Price>(opt.match_levels);
        o.price    = (o.side == Side::Buy) ? ref + reach : ref - reach;
        o.quantity = static_cast<Qty>(200 + (rng() % 200));
        w.crossing.push_back(o);
    }

    return w;
}

struct Measurement {
    HdrHistogram hist{3'600'000'000ULL, 3};
    double       wall_seconds = 0.0;
    std::size_t  operations   = 0;

    [[nodiscard]] double ops_per_sec() const {
        return wall_seconds > 0.0 ? static_cast<double>(operations) / wall_seconds : 0.0;
    }
};

// --- individual measurements ------------------------------------------------

Measurement measure_insert(EngineVersion v, const Options& opt, const Workload& w) {
    Measurement m;
    auto book = make_book(v, bench_config());
    EventSink sink(16);

    const double ghz = tsc_ghz();

    for (std::size_t i = 0; i < opt.warmup && i < w.passive.size(); ++i) {
        sink.clear();
        book->submit(w.passive[i], sink);
    }

    const auto wall0 = std::chrono::steady_clock::now();
    for (std::size_t i = opt.warmup; i < w.passive.size(); ++i) {
        sink.clear();
        NewOrder o = w.passive[i];
        do_not_optimize(o);

        const std::uint64_t t0 = rdtsc_now();
        SubmitResult r = book->submit(o, sink);
        const std::uint64_t t1 = rdtsc_now();

        do_not_optimize(r);
        m.hist.record(static_cast<std::uint64_t>(static_cast<double>(t1 - t0) / ghz));
        ++m.operations;
    }
    const auto wall1 = std::chrono::steady_clock::now();
    m.wall_seconds = std::chrono::duration<double>(wall1 - wall0).count();
    return m;
}

Measurement measure_cancel(EngineVersion v, const Options& opt, const Workload& w) {
    Measurement m;
    auto book = make_book(v, bench_config());
    EventSink sink(16);
    const double ghz = tsc_ghz();

    // Fill the book first: cancelling an empty book measures the rejection
    // path, not the cancel path.
    std::vector<ClientOrderId> resting;
    resting.reserve(w.passive.size());
    for (const NewOrder& o : w.passive) {
        sink.clear();
        const SubmitResult r = book->submit(o, sink);
        if (r.resting) resting.push_back(o.client_id);
    }

    std::size_t idx = 0;
    for (; idx < opt.warmup && idx < resting.size(); ++idx) {
        sink.clear();
        book->cancel(resting[idx], 0, sink);  // participant 0 mismatches: rejection path
    }

    const auto wall0 = std::chrono::steady_clock::now();
    for (; idx < resting.size(); ++idx) {
        sink.clear();
        const ClientOrderId id = resting[idx];

        const std::uint64_t t0 = rdtsc_now();
        // Participant is looked up from the workload so the cancel succeeds;
        // a rejected cancel exercises a much shorter path and would flatter the
        // result.
        CancelResult r = book->cancel(id, w.passive[idx].participant, sink);
        const std::uint64_t t1 = rdtsc_now();

        do_not_optimize(r);
        m.hist.record(static_cast<std::uint64_t>(static_cast<double>(t1 - t0) / ghz));
        ++m.operations;
    }
    const auto wall1 = std::chrono::steady_clock::now();
    m.wall_seconds = std::chrono::duration<double>(wall1 - wall0).count();
    return m;
}

Measurement measure_match(EngineVersion v, const Options& opt, const Workload& w) {
    Measurement m;
    auto book = make_book(v, bench_config());
    EventSink sink(64);
    const double ghz = tsc_ghz();

    // A crossing order consumes liquidity, so the book has to be refilled as
    // the measurement proceeds or the later iterations measure an empty book.
    // Refill happens outside the timed region.
    std::size_t passive_cursor = 0;
    auto refill = [&](std::size_t n) {
        for (std::size_t k = 0; k < n && passive_cursor < w.passive.size(); ++k, ++passive_cursor) {
            sink.clear();
            book->submit(w.passive[passive_cursor], sink);
        }
    };

    refill(opt.book_depth * 40);

    for (std::size_t i = 0; i < opt.warmup && i < w.crossing.size(); ++i) {
        sink.clear();
        book->submit(w.crossing[i], sink);
        refill(8);
    }

    const auto wall0 = std::chrono::steady_clock::now();
    for (std::size_t i = opt.warmup; i < w.crossing.size(); ++i) {
        NewOrder o = w.crossing[i];
        sink.clear();
        do_not_optimize(o);

        const std::uint64_t t0 = rdtsc_now();
        SubmitResult r = book->submit(o, sink);
        const std::uint64_t t1 = rdtsc_now();

        do_not_optimize(r);
        m.hist.record(static_cast<std::uint64_t>(static_cast<double>(t1 - t0) / ghz));
        ++m.operations;

        // Refill outside the timed region. If the pool of passive orders runs
        // out the book drains and the remaining samples measure a thin book;
        // the generated workload is sized to make that unlikely.
        refill(8);
    }
    const auto wall1 = std::chrono::steady_clock::now();
    m.wall_seconds = std::chrono::duration<double>(wall1 - wall0).count();
    return m;
}

// --- reporting --------------------------------------------------------------

struct RepStats {
    std::vector<std::uint64_t> p50, p99, p999;
    std::vector<double>        throughput;

    void add(const Measurement& m) {
        p50.push_back(m.hist.value_at_percentile(50.0));
        p99.push_back(m.hist.value_at_percentile(99.0));
        p999.push_back(m.hist.value_at_percentile(99.9));
        throughput.push_back(m.ops_per_sec());
    }
};

template <typename T>
T median_of(std::vector<T> v) {
    if (v.empty()) return T{};
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

template <typename T>
double spread_pct(const std::vector<T>& v) {
    if (v.size() < 2) return 0.0;
    const auto [mn, mx] = std::minmax_element(v.begin(), v.end());
    const double med = static_cast<double>(median_of(v));
    if (med <= 0.0) return 0.0;
    return 100.0 * (static_cast<double>(*mx) - static_cast<double>(*mn)) / med;
}

void write_op_block(std::ofstream& out, const char* version, const char* op,
                    const RepStats& rs) {
    out << version << " " << op << " p50_ns "  << median_of(rs.p50)  << "\n";
    out << version << " " << op << " p99_ns "  << median_of(rs.p99)  << "\n";
    out << version << " " << op << " p999_ns " << median_of(rs.p999) << "\n";
    out << version << " " << op << " throughput_ops_per_sec "
        << static_cast<std::uint64_t>(median_of(rs.throughput)) << "\n";
    // The spread across repetitions is the error bar. A delta between versions
    // smaller than this is not a result.
    out << version << " " << op << " p50_spread_pct "
        << spread_pct(rs.p50) << "\n";
    out << version << " " << op << " throughput_spread_pct "
        << spread_pct(rs.throughput) << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--iterations")        opt.iterations = std::strtoull(next(), nullptr, 10);
        else if (a == "--warmup")       opt.warmup     = std::strtoull(next(), nullptr, 10);
        else if (a == "--reps")         opt.reps       = std::strtoull(next(), nullptr, 10);
        else if (a == "--depth")        opt.book_depth = std::strtoull(next(), nullptr, 10);
        else if (a == "--match-levels") opt.match_levels = std::strtoull(next(), nullptr, 10);
        else if (a == "--out")          opt.out_dir    = next();
        else if (a == "--seed")         opt.seed       = std::strtoull(next(), nullptr, 10);
        else if (a == "--versions") {
            opt.versions.clear();
            std::string list = next();
            std::size_t pos = 0;
            while (pos <= list.size()) {
                const std::size_t comma = list.find(',', pos);
                const std::string name = list.substr(pos, comma - pos);
                EngineVersion v;
                if (!name.empty() && parse_engine_version(name, v)) opt.versions.push_back(v);
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        } else if (a == "--help" || a == "-h") {
            std::printf(
                "matchbook-bench [options]\n"
                "  --iterations N    timed operations per repetition (default 200000)\n"
                "  --warmup N        discarded operations before timing (default 20000)\n"
                "  --reps N          independent repetitions (default 5)\n"
                "  --depth N         price levels pre-populated per side (default 20)\n"
                "  --match-levels N  levels a crossing order sweeps (default 5)\n"
                "  --versions LIST   comma-separated: v0,v1,v2,v3\n"
                "  --out DIR         results directory (default bench/results)\n"
                "  --seed N          workload RNG seed\n"
                "\nRun under taskset for stable numbers:\n"
                "  taskset -c 2 ./build/matchbook-bench\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument '%s' (try --help)\n", a.c_str());
            return 2;
        }
    }

    if (opt.reps < 5) {
        std::fprintf(stderr,
                     "[warn] --reps %zu is below the documented minimum of 5; the\n"
                     "       inter-run spread will not be meaningful.\n", opt.reps);
    }

    const MachineFingerprint fp = capture_fingerprint();
    std::printf("%s\n", fp.to_text().c_str());
    std::printf("generating workload (seed=%llu)...\n",
                static_cast<unsigned long long>(opt.seed));
    const Workload workload = generate(opt);

    std::error_code ec;
    std::filesystem::create_directories(opt.out_dir, ec);

    const std::string path = opt.out_dir + "/bench-" + fp.timestamp_utc + ".txt";
    std::ofstream out(path);
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return 1;
    }

    out << fp.to_text();
    out << "# iterations " << opt.iterations << "\n";
    out << "# warmup " << opt.warmup << "\n";
    out << "# reps " << opt.reps << "\n";
    out << "# book_depth " << opt.book_depth << "\n";
    out << "# match_levels " << opt.match_levels << "\n";
    out << "# seed " << opt.seed << "\n";
    out << "# format: <version> <operation> <metric> <value>\n";

    for (EngineVersion v : opt.versions) {
        const char* name = to_string(v).data();
        std::printf("=== %s ===\n", name);

        RepStats insert_rs, cancel_rs, match_rs;
        for (std::size_t rep = 0; rep < opt.reps; ++rep) {
            std::printf("  rep %zu/%zu ... ", rep + 1, opt.reps);
            std::fflush(stdout);

            insert_rs.add(measure_insert(v, opt, workload));
            cancel_rs.add(measure_cancel(v, opt, workload));
            match_rs.add(measure_match(v, opt, workload));

            std::printf("done\n");
        }

        write_op_block(out, name, "insert", insert_rs);
        write_op_block(out, name, "cancel", cancel_rs);
        write_op_block(out, name, "match",  match_rs);

        std::printf("  insert  p50=%lluns p99=%lluns  throughput=%.0f ops/s\n",
                    static_cast<unsigned long long>(median_of(insert_rs.p50)),
                    static_cast<unsigned long long>(median_of(insert_rs.p99)),
                    median_of(insert_rs.throughput));
        std::printf("  cancel  p50=%lluns p99=%lluns\n",
                    static_cast<unsigned long long>(median_of(cancel_rs.p50)),
                    static_cast<unsigned long long>(median_of(cancel_rs.p99)));
        std::printf("  match   p50=%lluns p99=%lluns\n",
                    static_cast<unsigned long long>(median_of(match_rs.p50)),
                    static_cast<unsigned long long>(median_of(match_rs.p99)));
    }

    out.flush();
    std::printf("\nwrote %s\n", path.c_str());
    std::printf("generate the report with:  python3 tools/make_report.py\n");
    return 0;
}
