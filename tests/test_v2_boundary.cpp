// V2/V3 flat price-array boundary behaviour.
//
// The flat array can only represent prices in [base, base + levels*tick]. What
// happens at and past those edges is the single most important thing to get
// right about this design, because the failure mode of getting it wrong is not
// a crash -- it is a silent wrap-around that puts an order at the wrong price.

#include "test_helpers.hpp"

using namespace mb_test;

namespace {

// A deliberately small array so the boundaries are easy to reach in a test.
BookConfig narrow_config() {
    BookConfig cfg = default_config();
    cfg.tick_size        = 1;
    cfg.reference_price  = 1'000;
    cfg.flat_levels      = 256;      // base = 1000 - 128 = 872, top = 1127
    cfg.price_band_ticks = 100'000;  // wide, so the band gate does not mask the array gate
    return cfg;
}

const std::vector<EngineVersion>& flat_versions() {
    static const std::vector<EngineVersion> v = {
        EngineVersion::V2_Flat, EngineVersion::V3_Tuned,
    };
    return v;
}

}  // namespace

TEST_CASE("flat array computes its own bounds consistently", "[v2][boundary]") {
    const BookConfig cfg = narrow_config();
    CHECK(cfg.flat_base_price() == 1'000 - 128);
    CHECK(cfg.flat_top_price() == cfg.flat_base_price() + 255);
}

TEST_CASE("an order at the exact array base is accepted", "[v2][boundary]") {
    const BookConfig cfg = narrow_config();
    for (EngineVersion v : flat_versions()) {
        INFO("engine = " << to_string(v));
        auto book = make_book(v, cfg);
        EventSink sink;

        const SubmitResult r = book->submit(limit(1, Side::Buy, cfg.flat_base_price(), 10, 1), sink);
        CHECK(r.accepted);
        CHECK(r.resting);

        Price bid = 0;
        REQUIRE(book->best_bid(bid));
        CHECK(bid == cfg.flat_base_price());
        require_consistent(*book);
    }
}

TEST_CASE("an order at the exact array top is accepted", "[v2][boundary]") {
    const BookConfig cfg = narrow_config();
    for (EngineVersion v : flat_versions()) {
        INFO("engine = " << to_string(v));
        auto book = make_book(v, cfg);
        EventSink sink;

        const SubmitResult r = book->submit(limit(1, Side::Sell, cfg.flat_top_price(), 10, 1), sink);
        CHECK(r.accepted);

        Price ask = 0;
        REQUIRE(book->best_ask(ask));
        CHECK(ask == cfg.flat_top_price());
        require_consistent(*book);
    }
}

TEST_CASE("a resting order below the array base is rejected, not wrapped",
          "[v2][boundary]") {
    const BookConfig cfg = narrow_config();
    for (EngineVersion v : flat_versions()) {
        INFO("engine = " << to_string(v));
        auto book = make_book(v, cfg);
        EventSink sink;

        const SubmitResult r = book->submit(limit(1, Side::Buy, cfg.flat_base_price() - 1, 10, 1), sink);
        CHECK_FALSE(r.accepted);
        CHECK(r.reason == RejectReason::PriceOutsideArray);
        CHECK(book->resting_count() == 0);
        require_consistent(*book);
    }
}

TEST_CASE("a resting order above the array top is rejected, not wrapped",
          "[v2][boundary]") {
    const BookConfig cfg = narrow_config();
    for (EngineVersion v : flat_versions()) {
        INFO("engine = " << to_string(v));
        auto book = make_book(v, cfg);
        EventSink sink;

        const SubmitResult r = book->submit(limit(1, Side::Sell, cfg.flat_top_price() + 1, 10, 1), sink);
        CHECK_FALSE(r.accepted);
        CHECK(r.reason == RejectReason::PriceOutsideArray);
        CHECK(book->resting_count() == 0);
        require_consistent(*book);
    }
}

TEST_CASE("a price not on a tick boundary is rejected by the array mapping",
          "[v2][boundary]") {
    BookConfig cfg = narrow_config();
    cfg.tick_size       = 5;
    cfg.reference_price = 1'000;

    for (EngineVersion v : flat_versions()) {
        INFO("engine = " << to_string(v));
        auto book = make_book(v, cfg);
        EventSink sink;

        // 1002 is not a multiple of 5 relative to the base, so it has no index.
        const SubmitResult r = book->submit(limit(1, Side::Buy, 1'002, 10, 1), sink);
        CHECK_FALSE(r.accepted);
        // Either gate is a correct answer; what matters is that it does not rest.
        CHECK((r.reason == RejectReason::PriceOutsideArray ||
               r.reason == RejectReason::PriceNotOnTick));
        CHECK(book->resting_count() == 0);
        require_consistent(*book);
    }
}

TEST_CASE("a MARKET order is unaffected by the array bounds", "[v2][boundary]") {
    const BookConfig cfg = narrow_config();
    for (EngineVersion v : flat_versions()) {
        INFO("engine = " << to_string(v));
        auto book = make_book(v, cfg);
        EventSink sink;

        book->submit(limit(1, Side::Sell, 1'000, 20, 1), sink);
        sink.clear();

        // MARKET never rests, so it never needs an index.
        const SubmitResult r = book->submit(market(2, Side::Buy, 20, 2), sink);
        CHECK(r.accepted);
        CHECK(r.filled_qty == 20);
        require_consistent(*book);
    }
}

TEST_CASE("orders at both extremes coexist without aliasing", "[v2][boundary]") {
    const BookConfig cfg = narrow_config();
    for (EngineVersion v : flat_versions()) {
        INFO("engine = " << to_string(v));
        auto book = make_book(v, cfg);
        EventSink sink;

        book->submit(limit(1, Side::Buy,  cfg.flat_base_price(), 10, 1), sink);
        book->submit(limit(2, Side::Sell, cfg.flat_top_price(),  10, 2), sink);
        sink.clear();

        Price bid = 0, ask = 0;
        REQUIRE(book->best_bid(bid));
        REQUIRE(book->best_ask(ask));
        CHECK(bid == cfg.flat_base_price());
        CHECK(ask == cfg.flat_top_price());
        CHECK(bid < ask);
        CHECK(book->resting_count() == 2);
        require_consistent(*book);
    }
}

TEST_CASE("the bitmap finds the best level across a sparse array", "[v2][boundary]") {
    const BookConfig cfg = narrow_config();
    for (EngineVersion v : flat_versions()) {
        INFO("engine = " << to_string(v));
        auto book = make_book(v, cfg);
        EventSink sink;

        // Two bids far apart: one at the very bottom, one near the reference.
        book->submit(limit(1, Side::Buy, cfg.flat_base_price(), 10, 1), sink);
        book->submit(limit(2, Side::Buy, 999, 10, 2), sink);
        sink.clear();

        Price bid = 0;
        REQUIRE(book->best_bid(bid));
        CHECK(bid == 999);   // the higher of the two, not the first inserted

        // Cancel the best and the next-best must surface.
        REQUIRE(book->cancel(2, 2, sink).accepted);
        REQUIRE(book->best_bid(bid));
        CHECK(bid == cfg.flat_base_price());

        require_consistent(*book);
    }
}

TEST_CASE("V0 and V1 accept prices the flat array cannot represent",
          "[v2][boundary][design]") {
    // This is the documented divergence, asserted rather than left implicit.
    // A price outside the configured window is perfectly valid to a tree-based
    // book and impossible for a flat one. The README calls this out as the case
    // where V2's design is the wrong choice.
    BookConfig cfg = narrow_config();
    const Price far_price = cfg.flat_top_price() + 50;

    for (EngineVersion v : {EngineVersion::V0_Naive, EngineVersion::V1_Pool}) {
        INFO("engine = " << to_string(v));
        auto book = make_book(v, cfg);
        EventSink sink;
        const SubmitResult r = book->submit(limit(1, Side::Sell, far_price, 10, 1), sink);
        CHECK(r.accepted);
        CHECK(r.resting);
        require_consistent(*book);
    }

    for (EngineVersion v : flat_versions()) {
        INFO("engine = " << to_string(v));
        auto book = make_book(v, cfg);
        EventSink sink;
        const SubmitResult r = book->submit(limit(1, Side::Sell, far_price, 10, 1), sink);
        CHECK_FALSE(r.accepted);
        CHECK(r.reason == RejectReason::PriceOutsideArray);
    }
}
