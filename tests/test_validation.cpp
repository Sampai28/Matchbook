// The inbound validation gate and the invariant checker.
//
// Every one of these paths must reject and continue. None may assert, throw, or
// abort in a release build: a malformed message from one participant must not
// be able to take the venue down.

#include <limits>

#include "test_helpers.hpp"
#include "validation/invariants.hpp"
#include "validation/validator.hpp"

using namespace mb_test;

namespace {

matchbook::Validator make_validator() {
    return matchbook::Validator(default_config());
}

}  // namespace

TEST_CASE("a well-formed limit order passes validation", "[validation]") {
    auto v = make_validator();
    CHECK(v.validate_new(limit(1, Side::Buy, 100'000, 10, 1)) == RejectReason::None);
}

TEST_CASE("non-positive quantity is rejected", "[validation]") {
    auto v = make_validator();
    CHECK(v.validate_new(limit(1, Side::Buy, 100'000, 0, 1)) == RejectReason::InvalidQuantity);
    CHECK(v.validate_new(limit(2, Side::Buy, 100'000, -5, 1)) == RejectReason::InvalidQuantity);
}

TEST_CASE("non-positive price is rejected", "[validation]") {
    auto v = make_validator();
    CHECK(v.validate_new(limit(1, Side::Buy, 0, 10, 1)) == RejectReason::InvalidPrice);
    CHECK(v.validate_new(limit(2, Side::Buy, -100, 10, 1)) == RejectReason::InvalidPrice);
}

TEST_CASE("overflowing price or quantity is rejected", "[validation][overflow]") {
    auto v = make_validator();

    NewOrder huge_qty = limit(1, Side::Buy, 100'000, std::numeric_limits<Qty>::max(), 1);
    CHECK(v.validate_new(huge_qty) == RejectReason::InvalidQuantity);

    NewOrder huge_price = limit(2, Side::Buy, std::numeric_limits<Price>::max(), 10, 1);
    CHECK(v.validate_new(huge_price) == RejectReason::InvalidPrice);

    // Individually fine, but the notional would overflow. This is the case a
    // naive implementation misses, because each field passes its own range
    // check and only the product is impossible.
    NewOrder combo = limit(3, Side::Buy, kMaxPrice - 1, kMaxQty - 1, 1);
    CHECK(v.validate_new(combo) != RejectReason::None);
}

TEST_CASE("price off the tick grid is rejected", "[validation]") {
    BookConfig cfg = default_config();
    cfg.tick_size = 25;
    matchbook::Validator v(cfg);

    CHECK(v.validate_new(limit(1, Side::Buy, 100'000, 10, 1)) == RejectReason::None);
    CHECK(v.validate_new(limit(2, Side::Buy, 100'013, 10, 1)) == RejectReason::PriceNotOnTick);
}

TEST_CASE("price outside the band is rejected", "[validation]") {
    BookConfig cfg = default_config();
    cfg.reference_price  = 100'000;
    cfg.price_band_ticks = 100;
    cfg.tick_size        = 1;
    matchbook::Validator v(cfg);

    CHECK(v.validate_new(limit(1, Side::Buy, 100'100, 10, 1)) == RejectReason::None);
    CHECK(v.validate_new(limit(2, Side::Buy, 100'101, 10, 1)) == RejectReason::PriceBandViolation);
    CHECK(v.validate_new(limit(3, Side::Buy,  99'899, 10, 1)) == RejectReason::PriceBandViolation);
}

TEST_CASE("a MARKET order carrying a price is rejected", "[validation]") {
    auto v = make_validator();
    NewOrder o = market(1, Side::Buy, 10, 1);
    CHECK(v.validate_new(o) == RejectReason::None);

    o.price = 100'000;
    CHECK(v.validate_new(o) == RejectReason::MarketOrderWithPrice);
}

TEST_CASE("a LIMIT order without a price is rejected", "[validation]") {
    auto v = make_validator();
    NewOrder o = limit(1, Side::Buy, 100'000, 10, 1);
    o.price = kNoPrice;
    CHECK(v.validate_new(o) == RejectReason::LimitOrderWithoutPrice);
}

TEST_CASE("replace validation mirrors new-order validation", "[validation][replace]") {
    auto v = make_validator();

    ReplaceRequest rq;
    rq.original_client_id = 1;
    rq.new_client_id      = 2;
    rq.participant        = 1;
    rq.new_price          = 100'000;
    rq.new_quantity       = 10;
    CHECK(v.validate_replace(rq) == RejectReason::None);

    rq.new_quantity = 0;
    CHECK(v.validate_replace(rq) == RejectReason::InvalidQuantity);

    rq.new_quantity = 10;
    rq.new_price    = -1;
    CHECK(v.validate_replace(rq) == RejectReason::InvalidPrice);

    // kNoPrice means "leave the price alone", which is legal.
    rq.new_price = kNoPrice;
    CHECK(v.validate_replace(rq) == RejectReason::None);
}

TEST_CASE("unknown symbol is rejected at the engine level", "[validation][engine]") {
    matchbook::Engine engine(EngineVersion::V3_Tuned);
    engine.add_symbol(default_config());

    EventSink sink;
    OrderRequest req;
    req.symbol      = "NOSUCH";
    req.client_id   = 1;
    req.side        = Side::Buy;
    req.type        = OrdType::Limit;
    req.price       = 100'000;
    req.quantity    = 10;

    const SubmitResult r = engine.submit(req, sink);
    CHECK_FALSE(r.accepted);
    CHECK(r.reason == RejectReason::UnknownSymbol);
    CHECK(engine.gate_stats().rejects[static_cast<std::size_t>(RejectReason::UnknownSymbol)] == 1);
}

TEST_CASE("every rejection reason has a distinct name", "[validation][metrics]") {
    // The /stats endpoint keys counters by these strings. Two reasons sharing a
    // name would silently merge two counters into one.
    std::vector<std::string_view> names;
    for (std::size_t i = 0; i < kRejectReasonCount; ++i) {
        names.push_back(to_string(static_cast<RejectReason>(i)));
    }
    std::sort(names.begin(), names.end());
    const auto dup = std::adjacent_find(names.begin(), names.end());
    INFO("duplicate reason name: " << (dup == names.end() ? "" : *dup));
    CHECK(dup == names.end());
}

TEST_CASE("a healthy book passes its own invariants", "[invariants]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        for (ClientOrderId i = 1; i <= 20; ++i) {
            const Side side = (i % 2 == 0) ? Side::Buy : Side::Sell;
            const Price px = (side == Side::Buy) ? 99'990 - static_cast<Price>(i)
                                                 : 100'010 + static_cast<Price>(i);
            book.submit(limit(i, side, px, 10 * static_cast<Qty>(i),
                              static_cast<ParticipantId>(i)), sink);
        }
        require_consistent(book);
    });
}

TEST_CASE("conservation holds across a run of crossing trades", "[invariants][conservation]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;

        for (ClientOrderId i = 1; i <= 10; ++i) {
            book.submit(limit(i, Side::Sell, 100'000 + static_cast<Price>(i), 25,
                              static_cast<ParticipantId>(i)), sink);
        }
        for (ClientOrderId i = 100; i < 110; ++i) {
            book.submit(limit(i, Side::Buy, 100'010, 30, 99), sink);
        }

        const Stats& s = book.stats();
        CHECK(s.buy_filled_qty == s.sell_filled_qty);
        CHECK(s.buy_notional == s.sell_notional);
        require_consistent(book);
    });
}

TEST_CASE("the invariant checker samples rather than checking every event",
          "[invariants][sampling]") {
    matchbook::InvariantChecker checker(InvariantMode::Sampled, /*period=*/10);
    auto book = make_book(EngineVersion::V1_Pool, default_config());
    Stats stats;

    for (int i = 0; i < 9; ++i) checker.after_event(*book, stats);
    CHECK(checker.checks_run() == 0);

    checker.after_event(*book, stats);
    CHECK(checker.checks_run() == 1);
}

TEST_CASE("sequence regressions are detected", "[invariants][sequence]") {
    matchbook::InvariantChecker checker(InvariantMode::Sampled, 1);
    Stats stats;

    CHECK(checker.observe_sequence(1, stats));
    CHECK(checker.observe_sequence(2, stats));
    CHECK(checker.observe_sequence(3, stats));

    CHECK_FALSE(checker.observe_sequence(3, stats));  // repeat
    CHECK(checker.violations() == 1);
    CHECK(stats.invariant_violations == 1);

    CHECK_FALSE(checker.observe_sequence(1, stats));  // regression
    CHECK(checker.violations() == 2);
}
