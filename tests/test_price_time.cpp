// Price-time priority, including under partial fills.
//
// This is the property an exchange is judged on. If it holds, the venue is
// fair; if it does not, someone is being queue-jumped, and no amount of speed
// compensates.

#include "test_helpers.hpp"

using namespace mb_test;

TEST_CASE("better price wins regardless of arrival order", "[priority]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;

        // Worse price arrives first.
        book.submit(limit(1, Side::Sell, 100'010, 50, /*participant=*/1), sink);
        sink.clear();
        book.submit(limit(2, Side::Sell, 100'005, 50, /*participant=*/2), sink);
        sink.clear();

        // A buyer crossing both should hit the cheaper ask first.
        book.submit(limit(3, Side::Buy, 100'010, 100, /*participant=*/3), sink);

        const auto fills = fills_of(sink);
        REQUIRE(fills.size() == 2);
        CHECK(fills[0].price == 100'005);
        CHECK(fills[0].maker_client_id == 2);
        CHECK(fills[1].price == 100'010);
        CHECK(fills[1].maker_client_id == 1);

        require_consistent(book);
    });
}

TEST_CASE("FIFO within a price level", "[priority]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;

        for (ClientOrderId id = 1; id <= 3; ++id) {
            book.submit(limit(id, Side::Sell, 100'000, 10,
                              static_cast<ParticipantId>(id)), sink);
            sink.clear();
        }

        book.submit(limit(99, Side::Buy, 100'000, 30, /*participant=*/9), sink);

        const auto fills = fills_of(sink);
        REQUIRE(fills.size() == 3);
        // Arrival order, not id order: they happen to coincide here, which is
        // what makes the assertion readable.
        CHECK(fills[0].maker_client_id == 1);
        CHECK(fills[1].maker_client_id == 2);
        CHECK(fills[2].maker_client_id == 3);

        require_consistent(book);
    });
}

TEST_CASE("a partial fill does not move the resting order in the queue", "[priority]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;

        book.submit(limit(1, Side::Sell, 100'000, 100, 1), sink);
        book.submit(limit(2, Side::Sell, 100'000, 100, 2), sink);
        sink.clear();

        // Take 40 from the front order, leaving it partially filled.
        book.submit(limit(10, Side::Buy, 100'000, 40, 9), sink);
        {
            const auto fills = fills_of(sink);
            REQUIRE(fills.size() == 1);
            CHECK(fills[0].maker_client_id == 1);
            CHECK(fills[0].quantity == 40);
        }
        sink.clear();

        // The next taker must still hit order 1's remaining 60 before touching
        // order 2. A partial fill must not send an order to the back.
        book.submit(limit(11, Side::Buy, 100'000, 100, 9), sink);
        {
            const auto fills = fills_of(sink);
            REQUIRE(fills.size() == 2);
            CHECK(fills[0].maker_client_id == 1);
            CHECK(fills[0].quantity == 60);
            CHECK(fills[1].maker_client_id == 2);
            CHECK(fills[1].quantity == 40);
        }

        require_consistent(book);
    });
}

TEST_CASE("trades print at the resting order's price", "[priority]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;

        book.submit(limit(1, Side::Sell, 100'000, 50, 1), sink);
        sink.clear();

        // The buyer is willing to pay more, but the maker set the terms.
        book.submit(limit(2, Side::Buy, 100'050, 50, 2), sink);

        const auto fills = fills_of(sink);
        REQUIRE(fills.size() == 1);
        CHECK(fills[0].price == 100'000);

        require_consistent(book);
    });
}

TEST_CASE("a crossing order sweeps multiple levels in price order", "[priority]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;

        for (int i = 0; i < 5; ++i) {
            book.submit(limit(static_cast<ClientOrderId>(i + 1), Side::Sell,
                              100'000 + i, 10, static_cast<ParticipantId>(i + 1)), sink);
            sink.clear();
        }

        book.submit(limit(100, Side::Buy, 100'004, 50, 20), sink);

        const auto fills = fills_of(sink);
        REQUIRE(fills.size() == 5);
        for (std::size_t i = 0; i < fills.size(); ++i) {
            CHECK(fills[i].price == static_cast<Price>(100'000 + i));
        }
        CHECK(book.resting_count() == 0);

        require_consistent(book);
    });
}

TEST_CASE("book never crosses after a resting insert", "[priority][invariant]") {
    for_each_engine([](IBook& book, EngineVersion) {
        EventSink sink;

        book.submit(limit(1, Side::Buy, 99'990, 100, 1), sink);
        book.submit(limit(2, Side::Sell, 100'010, 100, 2), sink);
        sink.clear();

        Price bid = 0, ask = 0;
        REQUIRE(book.best_bid(bid));
        REQUIRE(book.best_ask(ask));
        CHECK(bid < ask);

        // An aggressive buy consumes the ask entirely rather than resting above
        // it, which is what keeps the book uncrossed.
        book.submit(limit(3, Side::Buy, 100'010, 100, 3), sink);
        REQUIRE_FALSE(book.best_ask(ask));

        require_consistent(book);
    });
}

TEST_CASE("all four engines agree on a mixed sequence", "[priority][differential]") {
    // A miniature differential test: the same operations through every engine
    // must produce byte-identical event logs. tools/reference_matcher.py does
    // this at scale against an independent implementation; this catches the
    // obvious cases without leaving the C++ test suite.
    std::vector<std::string> logs;

    for (EngineVersion v : all_versions()) {
        auto book = make_book(v, default_config());
        EventSink sink;

        book->submit(limit(1, Side::Sell, 100'010, 50, 1), sink);
        book->submit(limit(2, Side::Sell, 100'005, 30, 2), sink);
        book->submit(limit(3, Side::Buy,  99'995, 40, 3), sink);
        book->submit(limit(4, Side::Buy, 100'010, 60, 4), sink);
        book->cancel(3, 3, sink);
        book->submit(market(5, Side::Sell, 25, 5), sink);
        book->submit(typed(6, Side::Buy, OrdType::IOC, 100'020, 100, 6), sink);

        logs.push_back(sink.to_text());
        require_consistent(*book);
    }

    REQUIRE(logs.size() == 4);
    for (std::size_t i = 1; i < logs.size(); ++i) {
        INFO("v0 log:\n" << logs[0] << "\nv" << i << " log:\n" << logs[i]);
        CHECK(logs[i] == logs[0]);
    }
}
