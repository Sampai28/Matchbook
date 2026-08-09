#pragma once

// Shared test scaffolding.
//
// Every behavioural test runs against all four engines. That is the point of
// having four: a test that only exercises V0 proves nothing about the
// implementation that will actually ship.

#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "matchbook/book.hpp"
#include "matchbook/engine.hpp"

namespace mb_test {

using namespace matchbook;

inline BookConfig default_config() {
    BookConfig cfg;
    cfg.symbol           = "TEST";
    cfg.tick_size        = 1;
    cfg.reference_price  = 100'000;
    cfg.price_band_ticks = 10'000;
    cfg.max_orders       = 4096;
    cfg.flat_levels      = 8192;
    // Tests run with the strictest checking available. A test suite that runs
    // in the same mode as production would miss exactly the corruption the
    // paranoid checks exist to catch.
    cfg.invariant_mode   = InvariantMode::Paranoid;
    return cfg;
}

inline const std::vector<EngineVersion>& all_versions() {
    static const std::vector<EngineVersion> v = {
        EngineVersion::V0_Naive,
        EngineVersion::V1_Pool,
        EngineVersion::V2_Flat,
        EngineVersion::V3_Tuned,
    };
    return v;
}

// Runs `body` once per engine version, with a SECTION-like label so a failure
// names the version that produced it.
template <typename F>
void for_each_engine(F&& body) {
    for (EngineVersion v : all_versions()) {
        INFO("engine = " << to_string(v));
        auto book = make_book(v, default_config());
        body(*book, v);
    }
}

template <typename F>
void for_each_engine_cfg(const BookConfig& cfg, F&& body) {
    for (EngineVersion v : all_versions()) {
        INFO("engine = " << to_string(v));
        auto book = make_book(v, cfg);
        body(*book, v);
    }
}

// --- request builders ------------------------------------------------------

inline NewOrder limit(ClientOrderId id, Side side, Price price, Qty qty,
                      ParticipantId participant = 1) {
    NewOrder o;
    o.client_id = id; o.side = side; o.type = OrdType::Limit;
    o.price = price; o.quantity = qty; o.participant = participant;
    return o;
}

inline NewOrder typed(ClientOrderId id, Side side, OrdType type, Price price, Qty qty,
                      ParticipantId participant = 1) {
    NewOrder o;
    o.client_id = id; o.side = side; o.type = type;
    o.price = price; o.quantity = qty; o.participant = participant;
    return o;
}

inline NewOrder market(ClientOrderId id, Side side, Qty qty, ParticipantId participant = 1) {
    NewOrder o;
    o.client_id = id; o.side = side; o.type = OrdType::Market;
    o.price = kNoPrice; o.quantity = qty; o.participant = participant;
    return o;
}

// --- event inspection ------------------------------------------------------

inline std::vector<Event> fills_of(const EventSink& sink) {
    std::vector<Event> out;
    for (const Event& e : sink.events()) {
        if (e.kind == EventKind::Fill) out.push_back(e);
    }
    return out;
}

inline std::size_t count_kind(const EventSink& sink, EventKind k) {
    std::size_t n = 0;
    for (const Event& e : sink.events()) if (e.kind == k) ++n;
    return n;
}

inline bool has_reject(const EventSink& sink, RejectReason r) {
    for (const Event& e : sink.events()) {
        if (e.kind == EventKind::Reject && e.reason == r) return true;
    }
    return false;
}

// Asserts the book is internally consistent, printing the full dump on failure.
inline void require_consistent(const IBook& book) {
    std::string detail;
    const bool ok = book.check_invariants(detail);
    if (!ok) {
        INFO("invariant failure:\n" << detail << "\n" << book.debug_dump());
    }
    REQUIRE(ok);
}

}  // namespace mb_test
