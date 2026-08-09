#pragma once

// A two-level occupancy bitmap over the flat price array, used by V2 and V3 to
// answer "what is the best bid/ask" without scanning empty levels.
//
// The problem it solves: V2 replaces the std::map price ladder with a flat
// array indexed by ticks-from-base. Array indexing is O(1), but finding the
// *highest occupied* index in a 65,536-entry array is O(n) if you scan it.
//
// One level of bitmap turns 65,536 level probes into 1,024 word probes. A
// second level -- a summary word-set where bit j is set iff word j is non-zero
// -- turns that into 16 probes plus one. Both levels together are 8.25 KB and
// stay resident in L1/L2, so a best-bid lookup is a handful of cycles rather
// than a walk over cold memory.
//
// `__builtin_ctzll` / `__builtin_clzll` compile to single TZCNT/LZCNT
// instructions on Zen. They are GCC/Clang builtins, not standard C++; C++20's
// <bit> offers std::countr_zero / std::countl_zero as portable equivalents, and
// those are used here so the code stays standard.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace matchbook::detail {

class LevelBitmap {
public:
    explicit LevelBitmap(std::size_t bits)
        : bits_(bits),
          words_((bits + 63) / 64, 0),
          summary_((((bits + 63) / 64) + 63) / 64, 0) {}

    void clear_all() noexcept {
        std::fill(words_.begin(), words_.end(), 0ULL);
        std::fill(summary_.begin(), summary_.end(), 0ULL);
        popcount_ = 0;
    }

    [[nodiscard]] std::size_t bits() const noexcept { return bits_; }
    [[nodiscard]] bool empty() const noexcept { return popcount_ == 0; }
    [[nodiscard]] std::size_t count() const noexcept { return popcount_; }

    [[nodiscard]] bool test(std::size_t i) const noexcept {
        return (words_[i >> 6] >> (i & 63)) & 1ULL;
    }

    void set(std::size_t i) noexcept {
        const std::size_t w = i >> 6;
        const std::uint64_t mask = 1ULL << (i & 63);
        if ((words_[w] & mask) != 0) return;
        words_[w] |= mask;
        summary_[w >> 6] |= (1ULL << (w & 63));
        ++popcount_;
    }

    void clear(std::size_t i) noexcept {
        const std::size_t w = i >> 6;
        const std::uint64_t mask = 1ULL << (i & 63);
        if ((words_[w] & mask) == 0) return;
        words_[w] &= ~mask;
        if (words_[w] == 0) {
            // Only drop the summary bit once the whole word is empty.
            summary_[w >> 6] &= ~(1ULL << (w & 63));
        }
        --popcount_;
    }

    // Lowest set bit: the best ask, since asks are indexed by ascending price.
    [[nodiscard]] bool find_first(std::size_t& out) const noexcept {
        for (std::size_t s = 0; s < summary_.size(); ++s) {
            const std::uint64_t sw = summary_[s];
            if (sw == 0) continue;
            const std::size_t w = (s << 6) + static_cast<std::size_t>(std::countr_zero(sw));
            const std::uint64_t word = words_[w];
            if (word == 0) continue;  // defensive; summary should preclude this
            out = (w << 6) + static_cast<std::size_t>(std::countr_zero(word));
            return true;
        }
        return false;
    }

    // Highest set bit: the best bid, since bids are indexed by ascending price
    // too and the best bid is the highest price.
    [[nodiscard]] bool find_last(std::size_t& out) const noexcept {
        for (std::size_t s = summary_.size(); s-- > 0;) {
            const std::uint64_t sw = summary_[s];
            if (sw == 0) continue;
            const std::size_t bit_in_summary =
                63 - static_cast<std::size_t>(std::countl_zero(sw));
            const std::size_t w = (s << 6) + bit_in_summary;
            const std::uint64_t word = words_[w];
            if (word == 0) continue;
            const std::size_t bit = 63 - static_cast<std::size_t>(std::countl_zero(word));
            out = (w << 6) + bit;
            return true;
        }
        return false;
    }

    // Next set bit strictly above `from`, used when walking up the ask side
    // through consecutive price levels during a multi-level match.
    [[nodiscard]] bool find_next(std::size_t from, std::size_t& out) const noexcept {
        if (from + 1 >= bits_) return false;
        std::size_t i = from + 1;
        std::size_t w = i >> 6;

        // Mask off the bits below `i` in the starting word.
        std::uint64_t word = words_[w] & (~0ULL << (i & 63));
        if (word != 0) {
            out = (w << 6) + static_cast<std::size_t>(std::countr_zero(word));
            return true;
        }
        for (++w; w < words_.size(); ++w) {
            if (words_[w] == 0) continue;
            out = (w << 6) + static_cast<std::size_t>(std::countr_zero(words_[w]));
            return true;
        }
        return false;
    }

    // Next set bit strictly below `from`, for walking down the bid side.
    [[nodiscard]] bool find_prev(std::size_t from, std::size_t& out) const noexcept {
        if (from == 0) return false;
        std::size_t i = from - 1;
        std::size_t w = i >> 6;

        const unsigned shift = 63 - static_cast<unsigned>(i & 63);
        std::uint64_t word = words_[w] & (~0ULL >> shift);
        if (word != 0) {
            out = (w << 6) + (63 - static_cast<std::size_t>(std::countl_zero(word)));
            return true;
        }
        while (w-- > 0) {
            if (words_[w] == 0) continue;
            out = (w << 6) + (63 - static_cast<std::size_t>(std::countl_zero(words_[w])));
            return true;
        }
        return false;
    }

private:
    std::size_t bits_;
    std::vector<std::uint64_t> words_;
    std::vector<std::uint64_t> summary_;
    std::size_t popcount_ = 0;
};

}  // namespace matchbook::detail
