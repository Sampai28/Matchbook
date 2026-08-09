#include "invariants.hpp"

#include <cstdio>
#include <cstdlib>

namespace matchbook {

bool InvariantChecker::after_event(const IBook& book, Stats& stats) {
    if (mode_ == InvariantMode::Off) return true;

    if (mode_ == InvariantMode::Sampled) {
        // Sampling is a counter, not a random draw: a deterministic engine must
        // stay deterministic, and a random sample would make the checker itself
        // a source of run-to-run variation.
        if (++counter_ < period_) return true;
        counter_ = 0;
    }

    return force_check(book, stats);
}

bool InvariantChecker::force_check(const IBook& book, Stats& stats) {
    std::string detail;
    ++checks_run_;
    stats.invariant_checks++;

    if (book.check_invariants(detail)) return true;

    report(book, detail, stats);
    return false;
}

bool InvariantChecker::observe_sequence(SeqNum seq, Stats& stats) {
    if (seq <= last_seq_) {
        ++violations_;
        stats.invariant_violations++;
        last_detail_ = "sequence number regression: got " + std::to_string(seq) +
                       " after " + std::to_string(last_seq_) + "\n";
        if (mode_ == InvariantMode::Paranoid) {
            std::fprintf(stderr, "MATCHBOOK INVARIANT VIOLATION\n%s", last_detail_.c_str());
            std::fflush(stderr);
            std::abort();
        }
        return false;
    }
    last_seq_ = seq;
    return true;
}

void InvariantChecker::report(const IBook& book, const std::string& detail, Stats& stats) {
    ++violations_;
    stats.invariant_violations++;
    last_detail_ = detail;

    if (mode_ == InvariantMode::Paranoid) {
        // A corrupted order book must not be allowed to keep matching: every
        // subsequent fill would be built on state already known to be wrong.
        // Aborting with the full dump gives the developer the state at the
        // moment of divergence rather than whatever it decayed into afterwards.
        std::fprintf(stderr,
                     "==================================================\n"
                     "MATCHBOOK INVARIANT VIOLATION (paranoid mode)\n"
                     "==================================================\n"
                     "%s\n%s\n",
                     detail.c_str(), book.debug_dump().c_str());
        std::fflush(stderr);
        std::abort();
    }

    // Release: record and carry on. The counter is exported via /stats, so a
    // violation is visible without being fatal.
    std::fprintf(stderr, "[matchbook] invariant violation (#%llu): %s",
                 static_cast<unsigned long long>(violations_), detail.c_str());
}

}  // namespace matchbook
