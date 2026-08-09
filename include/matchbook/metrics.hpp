#pragma once

// Counters and a small latency histogram, exported by GET /stats.
//
// Every rejection path in the system increments exactly one slot of
// Stats::rejects, so a rejected order is never an unattributable event. The
// invariant checker increments invariant_violations rather than aborting when
// running in release mode.

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "matchbook/types.hpp"

namespace matchbook {

struct Stats {
    // --- flow -----------------------------------------------------------
    std::uint64_t orders_received  = 0;
    std::uint64_t orders_accepted  = 0;
    std::uint64_t orders_rejected  = 0;
    std::uint64_t orders_resting   = 0;  // currently on the book
    std::uint64_t cancels_received = 0;
    std::uint64_t cancels_accepted = 0;
    std::uint64_t replaces_received = 0;
    std::uint64_t replaces_accepted = 0;
    std::uint64_t fills            = 0;

    // --- conservation ----------------------------------------------------
    // Buy-side and sell-side filled quantity are accumulated separately so the
    // invariant checker can assert they are equal. Every fill increments both
    // by the same amount, so a divergence means the matching loop dropped or
    // double-counted a leg.
    std::uint64_t buy_filled_qty  = 0;
    std::uint64_t sell_filled_qty = 0;

    // Traded notional, accumulated as price*qty on each side. Both sides use
    // identical arithmetic, so even in the extreme case where this overflows,
    // it overflows identically on both sides and the reconciliation still holds.
    std::int64_t buy_notional  = 0;
    std::int64_t sell_notional = 0;

    // --- integrity -------------------------------------------------------
    std::uint64_t invariant_violations = 0;
    std::uint64_t invariant_checks     = 0;

    std::array<std::uint64_t, kRejectReasonCount> rejects{};

    void count_reject(RejectReason r) noexcept {
        rejects[static_cast<std::size_t>(r)]++;
        orders_rejected++;
    }

    void reset() noexcept { *this = Stats{}; }
};

// Fixed-bucket latency histogram for the /stats endpoint.
//
// This is not the benchmark instrument; bench/hdr_histogram.hpp is. This one
// exists so a running server can report percentiles cheaply, with a bounded
// memory footprint and no allocation on the recording path.
//
// Buckets are log-linear over nanoseconds: 64 buckets per power of two, from
// 1ns to ~1s. That yields roughly 1.5% relative error, which is far finer than
// anything a server-side percentile is used for.
class LatencyHistogram {
public:
    static constexpr int kSubBucketBits  = 6;   // 64 sub-buckets per octave
    static constexpr int kSubBucketCount = 1 << kSubBucketBits;
    static constexpr int kOctaves        = 32;  // 1ns .. ~4.3s
    static constexpr std::size_t kBuckets =
        static_cast<std::size_t>(kOctaves) * kSubBucketCount;

    LatencyHistogram() : counts_(kBuckets, 0) {}

    void record(std::uint64_t nanos) noexcept {
        const std::size_t idx = bucket_of(nanos);
        counts_[idx]++;
        total_++;
        if (nanos > max_) max_ = nanos;
        sum_ += nanos;
    }

    void reset() noexcept {
        std::fill(counts_.begin(), counts_.end(), 0u);
        total_ = 0;
        max_   = 0;
        sum_   = 0;
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return total_; }
    [[nodiscard]] std::uint64_t max() const noexcept { return max_; }

    [[nodiscard]] double mean() const noexcept {
        return total_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(total_);
    }

    // Linear scan over 2048 buckets. Called once per /stats request, never in
    // the hot path.
    [[nodiscard]] std::uint64_t percentile(double p) const noexcept {
        if (total_ == 0) return 0;
        const auto target = static_cast<std::uint64_t>(
            static_cast<double>(total_) * p / 100.0);
        std::uint64_t running = 0;
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            running += counts_[i];
            if (running >= target) return value_of(i);
        }
        return max_;
    }

private:
    static std::size_t bucket_of(std::uint64_t v) noexcept {
        if (v < static_cast<std::uint64_t>(kSubBucketCount)) {
            return static_cast<std::size_t>(v);
        }
        // Index of the highest set bit: which octave the value falls in.
        const int msb = 63 - __builtin_clzll(v);
        const int octave = msb - kSubBucketBits + 1;
        const std::uint64_t sub = (v >> octave) & (kSubBucketCount - 1);
        const std::size_t idx =
            static_cast<std::size_t>(octave + 1) * kSubBucketCount + sub;
        return idx < kBuckets ? idx : kBuckets - 1;
    }

    static std::uint64_t value_of(std::size_t idx) noexcept {
        const auto octave = static_cast<int>(idx / kSubBucketCount);
        const auto sub    = static_cast<std::uint64_t>(idx % kSubBucketCount);
        if (octave == 0) return sub;
        return (static_cast<std::uint64_t>(kSubBucketCount) + sub) << (octave - 1);
    }

    std::vector<std::uint64_t> counts_;
    std::uint64_t total_ = 0;
    std::uint64_t max_   = 0;
    std::uint64_t sum_   = 0;
};

}  // namespace matchbook
