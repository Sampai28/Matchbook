#pragma once

// Per-book configuration. Fixed at construction; nothing here changes while a
// book is live, so every engine can read these fields without synchronisation
// and the optimiser can hoist them out of loops.

#include <cstdint>
#include <string>

#include "matchbook/types.hpp"

namespace matchbook {

// How aggressively the invariant checker runs.
//
// The checks are O(orders) and would dominate the hot path if run unconditionally
// after every event, so the mode is a deliberate trade of confidence against
// throughput.
enum class InvariantMode : std::uint8_t {
    Off      = 0,  // benchmarks only
    Sampled  = 1,  // release default: every Nth event
    Paranoid = 2,  // after every event; aborts with a state dump on violation
};

struct BookConfig {
    std::string symbol = "TEST";

    // --- price grid -----------------------------------------------------
    // All prices are integers in units of the minimum price increment. A tick
    // size of 1 means every integer price is valid; a tick size of 25 means
    // only multiples of 25 are.
    Price tick_size = 1;

    // The band around which V2/V3 lay out their flat price array, and against
    // which the price-band validation gate measures.
    Price reference_price = 100'000;

    // Orders further than this many ticks from reference_price are rejected.
    // A real venue recalculates reference intraday; Matchbook fixes it at
    // construction, which is noted as a limitation in the README.
    std::int64_t price_band_ticks = 10'000;

    // --- capacity -------------------------------------------------------
    // Size of the pre-allocated order pool in V1-V3. Once exhausted, new orders
    // are rejected with BookFull rather than the pool growing: a reallocation
    // would invalidate every intrusive pointer in the book.
    std::size_t max_orders = 1u << 20;  // 1,048,576

    // Number of price levels in the V2/V3 flat array. Covers
    // [reference_price - half*tick, reference_price + half*tick].
    // Memory cost is flat_levels * sizeof(FlatLevel), paid up front whether the
    // levels are used or not. See docs/expected-performance.md for when this is
    // the wrong shape entirely.
    std::size_t flat_levels = 1u << 16;  // 65,536 levels

    // --- policy ---------------------------------------------------------
    StpPolicy     stp            = StpPolicy::CancelResting;
    InvariantMode invariant_mode = InvariantMode::Sampled;
    std::uint32_t invariant_sample_period = 1024;  // Sampled mode: check every Nth

    // Lowest price the flat array can represent.
    [[nodiscard]] Price flat_base_price() const noexcept {
        const Price half = static_cast<Price>(flat_levels / 2) * tick_size;
        const Price base = reference_price - half;
        return base > 0 ? base : tick_size;
    }

    [[nodiscard]] Price flat_top_price() const noexcept {
        return flat_base_price() +
               static_cast<Price>(flat_levels - 1) * tick_size;
    }

    [[nodiscard]] bool price_on_tick(Price p) const noexcept {
        return tick_size > 0 && (p % tick_size) == 0;
    }

    [[nodiscard]] bool price_in_band(Price p) const noexcept {
        const Price span = price_band_ticks * tick_size;
        return p >= reference_price - span && p <= reference_price + span;
    }
};

}  // namespace matchbook
