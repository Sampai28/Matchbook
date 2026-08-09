#pragma once

// A calibrated TSC (time-stamp counter) reader for sub-100ns measurement.
//
// Why not just std::chrono::steady_clock? On Linux/glibc, steady_clock resolves
// through clock_gettime(CLOCK_MONOTONIC), which is a vDSO call. That is fast --
// roughly 15-25ns -- but the operations being measured here are of the same
// order. Timing a 40ns insert with a 20ns clock means half the measurement is
// the instrument.
//
// __rdtsc() reads the CPU's cycle counter directly, at ~6-10 cycles. On any
// modern AMD or Intel part the TSC is invariant: it ticks at a constant rate
// regardless of the core's current frequency, so it measures wall time, not
// work. That is exactly what is wanted here, but it also means the counter's
// rate must be calibrated against a real clock once before it can be converted
// to nanoseconds.
//
// Caveats that matter for interpreting the results:
//
//   * rdtsc is not a serialising instruction. The CPU may reorder it relative
//     to surrounding work. rdtscp (used below) waits for prior instructions to
//     retire, which is the cheap correction; a full lfence is stricter but adds
//     its own cost to every sample.
//
//   * On WSL2 the TSC is passed through from the host, so it is usable, but the
//     hypervisor may occasionally steal time. That shows up as outliers in the
//     tail, not as a shift in the median, which is one more reason the harness
//     reports medians across repetitions rather than a single mean.

#include <chrono>
#include <cstdint>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
#  include <x86intrin.h>
#  define MATCHBOOK_HAVE_RDTSC 1
#else
#  define MATCHBOOK_HAVE_RDTSC 0
#endif

namespace matchbook::bench {

[[nodiscard]] inline std::uint64_t rdtsc_now() noexcept {
#if MATCHBOOK_HAVE_RDTSC
    unsigned aux;
    // rdtscp waits for previously issued instructions to retire before reading
    // the counter, which stops the timer from floating above the work.
    return __rdtscp(&aux);
#else
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// Cycles-per-nanosecond, measured against steady_clock.
//
// Calibration takes the requested wall time; 200ms is enough to get within a
// fraction of a percent and short enough not to be annoying. The result is
// cached in a function-local static, so the first call pays and the rest do not.
[[nodiscard]] inline double tsc_ghz(std::chrono::milliseconds window =
                                        std::chrono::milliseconds(200)) {
    static const double cached = [window] {
#if MATCHBOOK_HAVE_RDTSC
        const auto wall_start = std::chrono::steady_clock::now();
        const std::uint64_t tsc_start = rdtsc_now();

        std::this_thread::sleep_for(window);

        const std::uint64_t tsc_end = rdtsc_now();
        const auto wall_end = std::chrono::steady_clock::now();

        const auto elapsed_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(wall_end - wall_start).count();
        if (elapsed_ns <= 0) return 1.0;
        return static_cast<double>(tsc_end - tsc_start) / static_cast<double>(elapsed_ns);
#else
        return 1.0;  // steady_clock fallback already counts nanoseconds
#endif
    }();
    return cached;
}

[[nodiscard]] inline double cycles_to_ns(std::uint64_t cycles) {
    return static_cast<double>(cycles) / tsc_ghz();
}

// Prevents the optimiser from deleting work whose result is unused.
//
// A benchmark that computes a value and throws it away is a benchmark the
// compiler is entitled to delete entirely, and a loop that measures nothing
// looks impressively fast. The empty asm block tells GCC that `value` may have
// been read and modified by code it cannot see, so it must actually exist.
template <typename T>
inline void do_not_optimize(T& value) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : "+r,m"(value) : : "memory");
#else
    volatile T sink = value;
    (void)sink;
#endif
}

inline void compiler_barrier() {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : : "memory");
#endif
}

}  // namespace matchbook::bench
