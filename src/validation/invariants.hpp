#pragma once

// Book invariant checking.
//
// The engines each implement check_invariants(), which does the structural
// work. This class decides *when* to run it and what to do when it fails, so
// that policy lives in one place rather than being duplicated across four
// engines.
//
// Paranoid mode aborts with a full state dump. Release mode increments a
// counter and logs. Neither uses assert(), because assert() compiles away under
// NDEBUG and an integrity check that vanishes in the build you actually ship is
// worse than no check at all -- it produces false confidence.

#include <cstdint>
#include <string>

#include "matchbook/book.hpp"
#include "matchbook/config.hpp"
#include "matchbook/metrics.hpp"

namespace matchbook {

class InvariantChecker {
public:
    explicit InvariantChecker(InvariantMode mode, std::uint32_t sample_period)
        : mode_(mode),
          period_(sample_period == 0 ? 1 : sample_period) {}

    // Called after every engine event. Returns true when the book is
    // consistent, or when this call was skipped by sampling.
    bool after_event(const IBook& book, Stats& stats);

    // Unconditional full check, ignoring the sampling mode. Used by tests and
    // by the /stats endpoint's optional deep check.
    bool force_check(const IBook& book, Stats& stats);

    [[nodiscard]] InvariantMode mode() const noexcept { return mode_; }
    void set_mode(InvariantMode m) noexcept { mode_ = m; }

    [[nodiscard]] std::uint64_t checks_run() const noexcept { return checks_run_; }
    [[nodiscard]] std::uint64_t violations() const noexcept { return violations_; }
    [[nodiscard]] const std::string& last_detail() const noexcept { return last_detail_; }

    // Sequence numbers must never go backwards or repeat. Tracked here rather
    // than in the engines because it is a property of the event stream, not of
    // the book's structure.
    bool observe_sequence(SeqNum seq, Stats& stats);

private:
    void report(const IBook& book, const std::string& detail, Stats& stats);

    InvariantMode  mode_;
    std::uint32_t  period_;
    std::uint32_t  counter_    = 0;
    std::uint64_t  checks_run_ = 0;
    std::uint64_t  violations_ = 0;
    SeqNum         last_seq_   = 0;
    std::string    last_detail_;
};

}  // namespace matchbook
