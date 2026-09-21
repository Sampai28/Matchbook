// Cancel and replace, including the paths that must reject rather than corrupt:
// unknown orders, terminal orders, and orders belonging to someone else.

#include "test_helpers.hpp"

using namespace mb_test;

TEST_CASE("cancel removes a resting order", "[cancel]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        book.submit(limit(1, Side::Buy, 99'990, 100, 1), sink);
        sink.clear();

        const CancelResult r = book.cancel(1, 1, sink);
        CHECK(r.accepted);
        CHECK(r.cancelled_qty == 100);
        CHECK(book.resting_count() == 0);

        Price bid = 0;
        CHECK_FALSE(book.best_bid(bid));
        require_consistent(book);
    });
}

TEST_CASE("cancel of an unknown order is rejected", "[cancel]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        const CancelResult r = book.cancel(4242, 1, sink);
        CHECK_FALSE(r.accepted);
        CHECK(r.reason == RejectReason::UnknownOrder);
        CHECK(has_reject(sink, RejectReason::UnknownOrder));
        require_consistent(book);
    });
}

TEST_CASE("cancel of an already-cancelled order is rejected", "[cancel][terminal]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        book.submit(limit(1, Side::Buy, 99'990, 100, 1), sink);
        REQUIRE(book.cancel(1, 1, sink).accepted);
        sink.clear();

        // The order is gone from the index, so the second cancel is reported as
        // unknown rather than terminal. Both are rejections; the distinction is
        // that the engine does not retain terminal orders, which is documented
        // behaviour and not an accident.
        const CancelResult r = book.cancel(1, 1, sink);
        CHECK_FALSE(r.accepted);
        CHECK(r.reason == RejectReason::UnknownOrder);
        require_consistent(book);
    });
}

TEST_CASE("cancel of a fully filled order is rejected", "[cancel][terminal]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        book.submit(limit(1, Side::Sell, 100'000, 50, 1), sink);
        book.submit(limit(2, Side::Buy, 100'000, 50, 2), sink);  // consumes order 1
        sink.clear();

        const CancelResult r = book.cancel(1, 1, sink);
        CHECK_FALSE(r.accepted);
        CHECK(r.reason == RejectReason::UnknownOrder);
        require_consistent(book);
    });
}

TEST_CASE("cancel by the wrong participant is rejected as unknown", "[cancel][security]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        book.submit(limit(1, Side::Buy, 99'990, 100, /*participant=*/7), sink);
        sink.clear();

        // Reported as unknown rather than "not yours". Confirming that someone
        // else's order exists is an information leak.
        const CancelResult r = book.cancel(1, /*participant=*/8, sink);
        CHECK_FALSE(r.accepted);
        CHECK(r.reason == RejectReason::UnknownOrder);
        CHECK(book.resting_count() == 1);
        require_consistent(book);
    });
}

TEST_CASE("replace that only reduces quantity keeps time priority", "[replace][priority]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        book.submit(limit(1, Side::Sell, 100'000, 100, 1), sink);
        book.submit(limit(2, Side::Sell, 100'000, 100, 2), sink);
        sink.clear();

        ReplaceRequest rq;
        rq.original_client_id = 1;
        rq.new_client_id      = 11;
        rq.participant        = 1;
        rq.new_price          = kNoPrice;   // unchanged
        rq.new_quantity       = 40;

        const ReplaceResult r = book.replace(rq, sink);
        CHECK(r.accepted);
        CHECK(r.priority_kept);
        sink.clear();

        // Order 1 must still be at the front of the queue with 40 left.
        book.submit(limit(50, Side::Buy, 100'000, 40, 9), sink);
        const auto fills = fills_of(sink);
        REQUIRE(fills.size() == 1);
        CHECK(fills[0].maker_client_id == 1);
        CHECK(fills[0].quantity == 40);

        require_consistent(book);
    });
}

TEST_CASE("replace that changes price loses time priority", "[replace][priority]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        book.submit(limit(1, Side::Sell, 100'000, 50, 1), sink);
        book.submit(limit(2, Side::Sell, 100'000, 50, 2), sink);
        sink.clear();

        // Move order 1 to a different price and back to the original. A price
        // change is the unambiguous case: the order is asking for a place in a
        // queue it was never in, so it joins at the back.
        ReplaceRequest away;
        away.original_client_id = 1;
        away.new_client_id      = 11;
        away.participant        = 1;
        away.new_price          = 100'100;
        away.new_quantity       = 50;
        const ReplaceResult moved = book.replace(away, sink);
        CHECK(moved.accepted);
        CHECK_FALSE(moved.priority_kept);
        sink.clear();

        ReplaceRequest back;
        back.original_client_id = 11;
        back.new_client_id      = 12;
        back.participant        = 1;
        back.new_price          = 100'000;
        back.new_quantity       = 50;
        const ReplaceResult returned = book.replace(back, sink);
        CHECK(returned.accepted);
        CHECK_FALSE(returned.priority_kept);
        sink.clear();

        book.submit(limit(60, Side::Buy, 100'000, 50, 9), sink);
        const auto fills = fills_of(sink);
        REQUIRE(fills.size() == 1);
        CHECK(fills[0].maker_client_id == 2);   // order 2 is now first

        require_consistent(book);
    });
}

TEST_CASE("replace at the same price and quantity keeps time priority",
          "[replace][priority]") {
    // The boundary case in the retention rule, pinned deliberately.
    //
    // Priority is kept when an amend does not *increase* quantity, not only
    // when it strictly decreases it. A resubmission at the same price and size
    // takes nothing from anyone queued behind, so demoting it would penalise a
    // client for sending a redundant message — no venue does that, and a client
    // retrying an amend it was unsure landed would silently lose its place.
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        book.submit(limit(1, Side::Sell, 100'000, 50, 1), sink);
        book.submit(limit(2, Side::Sell, 100'000, 50, 2), sink);
        sink.clear();

        ReplaceRequest rq;
        rq.original_client_id = 1;
        rq.new_client_id      = 11;
        rq.participant        = 1;
        rq.new_price          = 100'000;
        rq.new_quantity       = 50;
        const ReplaceResult r = book.replace(rq, sink);
        CHECK(r.accepted);
        CHECK(r.priority_kept);
        sink.clear();

        book.submit(limit(60, Side::Buy, 100'000, 50, 9), sink);
        const auto fills = fills_of(sink);
        REQUIRE(fills.size() == 1);
        CHECK(fills[0].maker_client_id == 1);   // order 1 kept its place

        require_consistent(book);
    });
}

TEST_CASE("replace of an unknown order is rejected", "[replace]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        ReplaceRequest rq;
        rq.original_client_id = 999;
        rq.new_client_id      = 1000;
        rq.participant        = 1;
        rq.new_price          = 100'000;
        rq.new_quantity       = 10;

        const ReplaceResult r = book.replace(rq, sink);
        CHECK_FALSE(r.accepted);
        CHECK(r.reason == RejectReason::UnknownOrder);
        require_consistent(book);
    });
}

TEST_CASE("replace of a filled order is rejected", "[replace][terminal]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        book.submit(limit(1, Side::Sell, 100'000, 50, 1), sink);
        book.submit(limit(2, Side::Buy, 100'000, 50, 2), sink);
        sink.clear();

        ReplaceRequest rq;
        rq.original_client_id = 1;
        rq.new_client_id      = 11;
        rq.participant        = 1;
        rq.new_price          = 100'000;
        rq.new_quantity       = 10;

        const ReplaceResult r = book.replace(rq, sink);
        CHECK_FALSE(r.accepted);
        CHECK(r.reason == RejectReason::UnknownOrder);
        require_consistent(book);
    });
}

TEST_CASE("replace by the wrong participant is rejected", "[replace][security]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        book.submit(limit(1, Side::Buy, 99'990, 100, /*participant=*/7), sink);
        sink.clear();

        ReplaceRequest rq;
        rq.original_client_id = 1;
        rq.new_client_id      = 11;
        rq.participant        = 8;
        rq.new_price          = 99'980;
        rq.new_quantity       = 50;

        const ReplaceResult r = book.replace(rq, sink);
        CHECK_FALSE(r.accepted);
        CHECK(r.reason == RejectReason::UnknownOrder);
        require_consistent(book);
    });
}

TEST_CASE("replace can reprice into a cross and fill immediately", "[replace]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;
        book.submit(limit(1, Side::Sell, 100'010, 50, 1), sink);
        book.submit(limit(2, Side::Buy,   99'990, 50, 2), sink);
        sink.clear();

        // Reprice the bid up through the ask.
        ReplaceRequest rq;
        rq.original_client_id = 2;
        rq.new_client_id      = 22;
        rq.participant        = 2;
        rq.new_price          = 100'010;
        rq.new_quantity       = 50;

        const ReplaceResult r = book.replace(rq, sink);
        CHECK(r.accepted);
        CHECK(r.filled_qty == 50);
        CHECK(count_kind(sink, EventKind::Fill) == 1);
        CHECK(book.resting_count() == 0);
        require_consistent(book);
    });
}

TEST_CASE("cancel and replace leave all engines in identical states",
          "[replace][differential]") {
    std::vector<std::string> logs;

    for (EngineVersion v : all_versions()) {
        auto book = make_book(v, default_config());
        EventSink sink;

        book->submit(limit(1, Side::Buy,  99'990, 100, 1), sink);
        book->submit(limit(2, Side::Buy,  99'990,  50, 2), sink);
        book->submit(limit(3, Side::Sell, 100'010, 80, 3), sink);

        ReplaceRequest shrink;
        shrink.original_client_id = 1;
        shrink.new_client_id      = 11;
        shrink.participant        = 1;
        shrink.new_price          = kNoPrice;
        shrink.new_quantity       = 60;
        book->replace(shrink, sink);

        book->cancel(2, 2, sink);

        ReplaceRequest reprice;
        reprice.original_client_id = 3;
        reprice.new_client_id      = 33;
        reprice.participant        = 3;
        reprice.new_price          = 99'990;
        reprice.new_quantity       = 80;
        book->replace(reprice, sink);

        logs.push_back(sink.to_text());
        require_consistent(*book);
    }

    for (std::size_t i = 1; i < logs.size(); ++i) {
        INFO("v0:\n" << logs[0] << "\nv" << i << ":\n" << logs[i]);
        CHECK(logs[i] == logs[0]);
    }
}
