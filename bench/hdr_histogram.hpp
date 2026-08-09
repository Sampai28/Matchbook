#pragma once

// A self-contained HdrHistogram.
//
// This implements the same algorithm as HdrHistogram_c (Gil Tene's
// high-dynamic-range histogram): values are bucketed by magnitude, with a fixed
// number of linear sub-buckets within each power-of-two range. That gives
// constant *relative* error across the whole range, so a 30ns sample and a 30ms
// sample are both recorded to the same precision in percentage terms -- which
// is exactly what a latency distribution spanning six orders of magnitude
// needs, and what a linear histogram cannot do.
//
// WHY NOT THE REAL LIBRARY: it is a fine library, and CMake can fetch it (set
// -DMATCHBOOK_USE_HDR_LIB=ON). It is not the default because the benchmark is
// the one component the developer runs *natively* under WSL2 rather than in
// Docker, and a FetchContent network dependency that fails there fails at the
// least convenient moment. This file is ~150 lines and has no dependencies.
//
// Precision is fixed at 3 significant figures (2048 sub-buckets), matching the
// HdrHistogram default. Memory is about 30-60KB for a nanosecond range, small
// enough to stay in L2 while recording.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace matchbook::bench {

class HdrHistogram {
public:
    // significant_figures: 1-5. 3 gives 0.1% relative error.
    explicit HdrHistogram(std::uint64_t highest_trackable = 3'600'000'000ULL,
                          int significant_figures = 3)
        : highest_(highest_trackable) {
        significant_figures = std::clamp(significant_figures, 1, 5);

        // Number of sub-buckets needed for the requested precision, rounded up
        // to a power of two so the index maths is shifts and masks.
        const auto largest_value_with_single_unit_resolution =
            static_cast<std::uint64_t>(2 * std::pow(10.0, significant_figures));
        int sub_bucket_count_magnitude = static_cast<int>(
            std::ceil(std::log2(static_cast<double>(largest_value_with_single_unit_resolution))));
        sub_bucket_half_count_magnitude_ =
            (sub_bucket_count_magnitude > 1 ? sub_bucket_count_magnitude : 1) - 1;

        sub_bucket_count_      = 1u << (sub_bucket_half_count_magnitude_ + 1);
        sub_bucket_half_count_ = sub_bucket_count_ / 2;
        sub_bucket_mask_       = static_cast<std::uint64_t>(sub_bucket_count_ - 1);

        // How many magnitude buckets are needed to reach the highest value.
        std::uint64_t smallest_untrackable = sub_bucket_count_;
        bucket_count_ = 1;
        while (smallest_untrackable < highest_) {
            smallest_untrackable <<= 1;
            ++bucket_count_;
        }

        counts_len_ = (bucket_count_ + 1) * sub_bucket_half_count_;
        counts_.assign(counts_len_, 0);
    }

    void record(std::uint64_t value) noexcept {
        const std::size_t idx = counts_index_for(value);
        if (idx >= counts_len_) {
            ++overflow_;
            return;
        }
        ++counts_[idx];
        ++total_;
        if (value > max_) max_ = value;
        if (total_ == 1 || value < min_) min_ = value;
        sum_ += value;
    }

    void reset() noexcept {
        std::fill(counts_.begin(), counts_.end(), 0ULL);
        total_ = 0; max_ = 0; min_ = 0; sum_ = 0; overflow_ = 0;
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return total_; }
    [[nodiscard]] std::uint64_t min() const noexcept { return total_ ? min_ : 0; }
    [[nodiscard]] std::uint64_t max() const noexcept { return max_; }
    [[nodiscard]] std::uint64_t overflow_count() const noexcept { return overflow_; }
    [[nodiscard]] double mean() const noexcept {
        return total_ ? static_cast<double>(sum_) / static_cast<double>(total_) : 0.0;
    }

    // The highest value at or below the given percentile. Walks the counts
    // array; O(buckets), called at report time only.
    [[nodiscard]] std::uint64_t value_at_percentile(double percentile) const noexcept {
        if (total_ == 0) return 0;
        const double p = std::clamp(percentile, 0.0, 100.0);
        auto target = static_cast<std::uint64_t>((p / 100.0) * static_cast<double>(total_) + 0.5);
        if (target == 0) target = 1;

        std::uint64_t running = 0;
        for (std::size_t i = 0; i < counts_len_; ++i) {
            running += counts_[i];
            if (running >= target) return highest_equivalent_value(value_from_index(i));
        }
        return max_;
    }

    [[nodiscard]] double stddev() const noexcept {
        if (total_ == 0) return 0.0;
        const double m = mean();
        double acc = 0.0;
        for (std::size_t i = 0; i < counts_len_; ++i) {
            if (counts_[i] == 0) continue;
            const double v = static_cast<double>(value_from_index(i));
            acc += (v - m) * (v - m) * static_cast<double>(counts_[i]);
        }
        return std::sqrt(acc / static_cast<double>(total_));
    }

    // Percentile rows, ready for a results file. Deliberately plain text: the
    // report generator parses this, and a text format survives being eyeballed
    // in a terminal when the generator is not available.
    [[nodiscard]] std::string percentile_table(const std::string& prefix) const {
        static const double kPoints[] = {50.0, 75.0, 90.0, 95.0, 99.0, 99.9, 99.99, 100.0};
        std::string out;
        for (double p : kPoints) {
            out += prefix;
            out += " p";
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", p);
            out += buf;
            out += " ";
            out += std::to_string(value_at_percentile(p));
            out += "\n";
        }
        return out;
    }

private:
    [[nodiscard]] int bucket_index_for(std::uint64_t value) const noexcept {
        // Leading-zero count gives the magnitude bucket in one instruction.
        const int leading = __builtin_clzll(value | sub_bucket_mask_);
        return 63 - leading - (sub_bucket_half_count_magnitude_ + 1) + 1;
    }

    [[nodiscard]] std::uint32_t sub_bucket_index_for(std::uint64_t value, int bucket) const noexcept {
        return static_cast<std::uint32_t>(value >> (bucket < 0 ? 0 : bucket));
    }

    [[nodiscard]] std::size_t counts_index_for(std::uint64_t value) const noexcept {
        int bucket = bucket_index_for(value);
        if (bucket < 0) bucket = 0;
        const std::uint32_t sub = sub_bucket_index_for(value, bucket);
        return counts_index(bucket, sub);
    }

    [[nodiscard]] std::size_t counts_index(int bucket, std::uint32_t sub) const noexcept {
        const std::size_t bucket_base = static_cast<std::size_t>(bucket + 1) * sub_bucket_half_count_;
        const std::size_t offset = (sub >= sub_bucket_half_count_)
                                       ? (sub - sub_bucket_half_count_)
                                       : sub;
        // For bucket 0 the sub-bucket index is used directly; for higher
        // buckets only the upper half is reachable, hence the offset.
        if (bucket == 0) return offset;
        return bucket_base + offset;
    }

    [[nodiscard]] std::uint64_t value_from_index(std::size_t index) const noexcept {
        std::size_t bucket = index / sub_bucket_half_count_;
        std::size_t sub    = (index % sub_bucket_half_count_) + sub_bucket_half_count_;
        if (bucket == 0) sub -= sub_bucket_half_count_;
        const int shift = static_cast<int>(bucket == 0 ? 0 : bucket - 1);
        return static_cast<std::uint64_t>(sub) << shift;
    }

    [[nodiscard]] std::uint64_t highest_equivalent_value(std::uint64_t value) const noexcept {
        // Every recorded value stands for a small range; the reported figure is
        // the top of that range, which is the conservative choice for latency.
        const int bucket = std::max(0, bucket_index_for(value));
        const int shift  = bucket;
        const std::uint64_t size = 1ULL << shift;
        return value + size - 1;
    }

    std::uint64_t highest_;
    int           sub_bucket_half_count_magnitude_ = 0;
    std::uint32_t sub_bucket_count_      = 0;
    std::uint32_t sub_bucket_half_count_ = 0;
    std::uint64_t sub_bucket_mask_       = 0;
    int           bucket_count_          = 0;
    std::size_t   counts_len_            = 0;

    std::vector<std::uint64_t> counts_;
    std::uint64_t total_    = 0;
    std::uint64_t min_      = 0;
    std::uint64_t max_      = 0;
    std::uint64_t sum_      = 0;
    std::uint64_t overflow_ = 0;
};

}  // namespace matchbook::bench
