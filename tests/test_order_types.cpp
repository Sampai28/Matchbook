// Semantics of each order type, including the two all-or-nothing rejections
// that are easiest to get subtly wrong: FOK and POST_ONLY.

#include "test_helpers.hpp"

using namespace mb_test;

TEST_CASE("LIMIT rests the unfilled remainder", "[types][limit]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        book.submit(limit(1, Side::Sell, 100'000, 30, 1), sink);
        sink.clear();

        const SubmitResult r = book.submit(limit(2, Side::Buy, 100'000, 100, 2), sink);
        CHECK(r.accepted);
        CHECK(r.filled_qty == 30);
        CHECK(r.remaining == 70);
        CHECK(r.resting);

        Price bid = 0;
        REQUIRE(book.best_bid(bid));
        CHECK(bid == 100'000);
        require_consistent(book);
    });
}

TEST_CASE("MARKET fills at any price and cancels the remainder", "[types][market]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        book.submit(limit(1, Side::Sell, 100'000, 20, 1), sink);
        book.submit(limit(2, Side::Sell, 100'050, 20, 2), sink);
        sink.clear();

        const SubmitResult r = book.submit(market(3, Side::Buy, 100, 3), sink);
        CHECK(r.accepted);
        CHECK(r.filled_qty == 40);
        CHECK(r.remaining == 60);
        CHECK_FALSE(r.resting);
        CHECK(count_kind(sink, EventKind::Cancel) == 1);

        // Nothing is left on either side: the asks were consumed and the market
        // order did not rest.
        CHECK(book.resting_count() == 0);
        require_consistent(book);
    });
}

TEST_CASE("MARKET against an empty book fills nothing", "[types][market]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        const SubmitResult r = book.submit(market(1, Side::Buy, 50, 1), sink);
        CHECK(r.accepted);        // accepted, then cancelled: not a rejection
        CHECK(r.filled_qty == 0);
        CHECK_FALSE(r.resting);
        CHECK(count_kind(sink, EventKind::Fill) == 0);
        require_consistent(book);
    });
}

TEST_CASE("IOC fills what it can and cancels the rest", "[types][ioc]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        book.submit(limit(1, Side::Sell, 100'000, 40, 1), sink);
        sink.clear();

        const SubmitResult r = book.submit(typed(2, Side::Buy, OrdType::IOC, 100'000, 100, 2), sink);
        CHECK(r.accepted);
        CHECK(r.filled_qty == 40);
        CHECK_FALSE(r.resting);
        CHECK(book.resting_count() == 0);
        require_consistent(book);
    });
}

TEST_CASE("IOC does not reach past its limit price", "[types][ioc]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        book.submit(limit(1, Side::Sell, 100'000, 10, 1), sink);
        book.submit(limit(2, Side::Sell, 100'100, 10, 2), sink);
        sink.clear();

        const SubmitResult r = book.submit(typed(3, Side::Buy, OrdType::IOC, 100'000, 50, 3), sink);
        CHECK(r.filled_qty == 10);   // only the level at or below its limit
        CHECK(book.resting_count() == 1);
        require_consistent(book);
    });
}

TEST_CASE("FOK is rejected when the full quantity is unavailable", "[types][fok]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        book.submit(limit(1, Side::Sell, 100'000, 40, 1), sink);
        sink.clear();

        const SubmitResult r = book.submit(typed(2, Side::Buy, OrdType::FOK, 100'000, 100, 2), sink);
        CHECK_FALSE(r.accepted);
        CHECK(r.reason == RejectReason::FokUnfillable);

        // The critical property: an unfillable FOK produces NO fills at all. A
        // partial fill followed by a rejection cannot be undone.
        CHECK(count_kind(sink, EventKind::Fill) == 0);
        CHECK(has_reject(sink, RejectReason::FokUnfillable));

        // And the book is untouched.
        CHECK(book.resting_count() == 1);
        require_consistent(book);
    });
}

TEST_CASE("FOK fills completely when the quantity is available", "[types][fok]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        book.submit(limit(1, Side::Sell, 100'000, 60, 1), sink);
        book.submit(limit(2, Side::Sell, 100'001, 60, 2), sink);
        sink.clear();

        const SubmitResult r = book.submit(typed(3, Side::Buy, OrdType::FOK, 100'001, 100, 3), sink);
        CHECK(r.accepted);
        CHECK(r.filled_qty == 100);
        CHECK(r.remaining == 0);
        CHECK(count_kind(sink, EventKind::Fill) == 2);
        require_consistent(book);
    });
}

TEST_CASE("FOK ignores the participant's own resting liquidity", "[types][fok][stp]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        // All the visible liquidity belongs to participant 7...
        book.submit(limit(1, Side::Sell, 100'000, 100, 7), sink);
        sink.clear();

        // ...so a FOK from participant 7 cannot use any of it and must be
        // rejected rather than trading with itself.
        const SubmitResult r = book.submit(typed(2, Side::Buy, OrdType::FOK, 100'000, 100, 7), sink);
        CHECK_FALSE(r.accepted);
        CHECK(r.reason == RejectReason::FokUnfillable);
        CHECK(count_kind(sink, EventKind::Fill) == 0);
        require_consistent(book);
    });
}

TEST_CASE("POST_ONLY is rejected when it would cross", "[types][postonly]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        book.submit(limit(1, Side::Sell, 100'000, 50, 1), sink);
        sink.clear();

        const SubmitResult r = book.submit(typed(2, Side::Buy, OrdType::PostOnly, 100'000, 10, 2), sink);
        CHECK_FALSE(r.accepted);
        CHECK(r.reason == RejectReason::PostOnlyWouldCross);
        CHECK(count_kind(sink, EventKind::Fill) == 0);
        CHECK(book.resting_count() == 1);   // only the original ask
        require_consistent(book);
    });
}

TEST_CASE("POST_ONLY rests when it does not cross", "[types][postonly]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        book.submit(limit(1, Side::Sell, 100'010, 50, 1), sink);
        sink.clear();

        const SubmitResult r = book.submit(typed(2, Side::Buy, OrdType::PostOnly, 100'000, 10, 2), sink);
        CHECK(r.accepted);
        CHECK(r.resting);
        CHECK(r.filled_qty == 0);

        Price bid = 0;
        REQUIRE(book.best_bid(bid));
        CHECK(bid == 100'000);
        require_consistent(book);
    });
}

TEST_CASE("POST_ONLY at exactly the touch is treated as crossing", "[types][postonly]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        book.submit(limit(1, Side::Sell, 100'000, 50, 1), sink);
        sink.clear();

        // A buy at the best ask would take, not make. Equal prices cross.
        const SubmitResult r = book.submit(typed(2, Side::Buy, OrdType::PostOnly, 100'000, 10, 2), sink);
        CHECK_FALSE(r.accepted);
        CHECK(r.reason == RejectReason::PostOnlyWouldCross);
        require_consistent(book);
    });
}

TEST_CASE("self-trade prevention cancels the resting order", "[types][stp]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        book.submit(limit(1, Side::Sell, 100'000, 50, /*participant=*/5), sink);
        sink.clear();

        // Participant 5 crosses its own order. The policy is CancelResting: the
        // resting order is cancelled and matching continues, so the aggressor
        // is not penalised for the collision.
        const SubmitResult r = book.submit(limit(2, Side::Buy, 100'000, 50, /*participant=*/5), sink);
        CHECK(r.accepted);
        CHECK(r.filled_qty == 0);
        CHECK(count_kind(sink, EventKind::Fill) == 0);
        CHECK(count_kind(sink, EventKind::Cancel) >= 1);

        // The aggressor rests; the victim is gone.
        Price bid = 0, ask = 0;
        CHECK(book.best_bid(bid));
        CHECK_FALSE(book.best_ask(ask));
        require_consistent(book);
    });
}

TEST_CASE("self-trade prevention does not stop matching against others",
          "[types][stp]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        book.submit(limit(1, Side::Sell, 100'000, 20, /*participant=*/5), sink);  // own
        book.submit(limit(2, Side::Sell, 100'000, 20, /*participant=*/6), sink);  // other
        sink.clear();

        const SubmitResult r = book.submit(limit(3, Side::Buy, 100'000, 20, /*participant=*/5), sink);
        CHECK(r.accepted);
        CHECK(r.filled_qty == 20);  // skipped its own, filled against 6

        const auto fills = fills_of(sink);
        REQUIRE(fills.size() == 1);
        CHECK(fills[0].maker_client_id == 2);
        require_consistent(book);
    });
}

TEST_CASE("duplicate client order id is rejected", "[types][validation]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        CHECK(book.submit(limit(1, Side::Buy, 99'000, 10, 1), sink).accepted);
        sink.clear();

        const SubmitResult r = book.submit(limit(1, Side::Buy, 99'000, 10, 1), sink);
        CHECK_FALSE(r.accepted);
        CHECK(r.reason == RejectReason::DuplicateClientOrderId);
        CHECK(book.resting_count() == 1);
        require_consistent(book);
    });
}
