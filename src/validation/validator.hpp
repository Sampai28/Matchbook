#pragma once

// Stateless inbound validation.
//
// Everything here can be decided from the request and the book's configuration
// alone. Checks that need book state -- duplicate client order id, unknown
// order, self-trade prevention -- live inside the engines, because that is
// where the relevant index is.
//
// The division matters: this class is trivially testable and fuzzable without
// constructing a book, which is what tests/fuzz_validation.cpp exploits.

#include <string>

#include "matchbook/config.hpp"
#include "matchbook/engine.hpp"
#include "matchbook/metrics.hpp"
#include "matchbook/order.hpp"
#include "matchbook/types.hpp"

namespace matchbook {

class Validator {
public:
    explicit Validator(const BookConfig& cfg) : cfg_(cfg) {}

    // Returns RejectReason::None when the request is acceptable. Never throws,
    // never asserts, and never reads uninitialised memory regardless of what
    // the caller passes in -- these are the properties the fuzz target checks.
    [[nodiscard]] RejectReason validate_new(const NewOrder& req) const noexcept;

    // Replace requests carry a price and quantity, so they get the same
    // treatment as a new order plus an identity check.
    [[nodiscard]] RejectReason validate_replace(const ReplaceRequest& req) const noexcept;

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    void count(RejectReason r) noexcept { stats_.count_reject(r); }

private:
    BookConfig cfg_;
    Stats      stats_;
};

}  // namespace matchbook
