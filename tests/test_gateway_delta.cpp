// Delta correctness: snapshot + replayed deltas must equal the engine's book.
//
// This is the test the gateway exists to be held to. A delta stream that drifts
// from the book fails silently — the ladder keeps rendering, the numbers are
// just wrong, and nothing in the system notices. Every other gateway bug
// announces itself; this one does not.
//
// The test drives the real wire path rather than the gateway's internal state:
// it subscribes, drains the SSE frames the subscriber actually received, parses
// them back out of the JSON, and applies them to a TrackedBook exactly as a
// conforming client would. That way the serialisation is under test too. A
// version that compared C++ structs would pass while the JSON writer emitted
// a misspelled field name.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>
#include <vector>

#include "server/gateway.hpp"
#include "test_helpers.hpp"

using namespace matchbook;
using namespace matchbook::gateway;
using namespace mb_test;

namespace {

// --- a deliberately small JSON scanner -------------------------------------
//
// The gateway's writer emits a known, flat, single-line shape, so scanning for
// field names is sufficient and keeps the test free of a JSON dependency. It is
// strict: a field that is missing or renamed makes the scan fail rather than
// silently yielding a default, which is the whole point of testing the wire
// format rather than the structs behind it.

bool field_i64(const std::string& s, std::size_t from, std::size_t to,
               std::string_view key, std::int64_t& out) {
    const std::string needle = "\"" + std::string(key) + "\":";
    const std::size_t at = s.find(needle, from);
    if (at == std::string::npos || at >= to) return false;
    out = std::strtoll(s.c_str() + at + needle.size(), nullptr, 10);
    return true;
}

bool field_str(const std::string& s, std::size_t from, std::size_t to,
               std::string_view key, std::string& out) {
    const std::string needle = "\"" + std::string(key) + "\":\"";
    const std::size_t at = s.find(needle, from);
    if (at == std::string::npos || at >= to) return false;
    const std::size_t start = at + needle.size();
    const std::size_t end   = s.find('"', start);
    if (end == std::string::npos) return false;
    out = s.substr(start, end - start);
    return true;
}

// The span of a named array, e.g. "levels":[ ... ]
bool array_span(const std::string& s, std::string_view key,
                std::size_t& begin, std::size_t& end) {
    const std::string needle = "\"" + std::string(key) + "\":[";
    const std::size_t at = s.find(needle);
    if (at == std::string::npos) return false;
    begin = at + needle.size();
    int depth = 1;
    for (std::size_t i = begin; i < s.size(); ++i) {
        if (s[i] == '[') ++depth;
        else if (s[i] == ']') {
            if (--depth == 0) { end = i; return true; }
        }
    }
    return false;
}

struct ParsedLevel {
    std::string   side;        // "BID" / "ASK", empty in a snapshot array
    std::string   taker_side;  // trades only
    std::int64_t  price    = 0;
    std::int64_t  quantity = 0;
    std::int64_t  orders   = 0;
};

// `require_orders` is false for the trades array, which carries price,
// quantity and takerSide but no order count.
std::vector<ParsedLevel> parse_objects(const std::string& s, std::string_view key,
                                       bool require_orders) {
    std::vector<ParsedLevel> out;
    std::size_t begin = 0, end = 0;
    if (!array_span(s, key, begin, end)) return out;

    std::size_t cursor = begin;
    while (cursor < end) {
        const std::size_t obj = s.find('{', cursor);
        if (obj == std::string::npos || obj >= end) break;
        const std::size_t obj_end = s.find('}', obj);
        if (obj_end == std::string::npos || obj_end > end) break;

        ParsedLevel lvl;
        field_str(s, obj, obj_end, "side", lvl.side);
        field_str(s, obj, obj_end, "takerSide", lvl.taker_side);
        REQUIRE(field_i64(s, obj, obj_end, "price", lvl.price));
        REQUIRE(field_i64(s, obj, obj_end, "quantity", lvl.quantity));
        if (require_orders) {
            REQUIRE(field_i64(s, obj, obj_end, "orders", lvl.orders));
        }
        out.push_back(lvl);
        cursor = obj_end + 1;
    }
    return out;
}

std::vector<ParsedLevel> parse_levels(const std::string& s, std::string_view key) {
    return parse_objects(s, key, /*require_orders=*/true);
}

std::vector<ParsedLevel> parse_trades(const std::string& s) {
    return parse_objects(s, "trades", /*require_orders=*/false);
}

std::int64_t seq_of(const std::string& frame) {
    std::int64_t seq = -1;
    REQUIRE(field_i64(frame, 0, frame.size(), "seq", seq));
    return seq;
}

std::string type_of(const std::string& frame) {
    std::string t;
    REQUIRE(field_str(frame, 0, frame.size(), "type", t));
    return t;
}

// Drains everything the subscriber has been handed since the last call.
std::vector<std::string> drain(const SubscriberPtr& sub) {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(sub->mu);
    while (!sub->queue.empty()) {
        out.push_back(sub->queue.front());
        sub->queue.pop_front();
    }
    return out;
}

// Seeds a TrackedBook from a snapshot frame, as a client does on connect.
void apply_snapshot(TrackedBook& tracked, const std::string& frame) {
    tracked.bids.clear();
    tracked.asks.clear();
    for (const ParsedLevel& l : parse_levels(frame, "bids")) {
        tracked.apply_level(Side::Buy, l.price, l.quantity,
                            static_cast<std::uint32_t>(l.orders));
    }
    for (const ParsedLevel& l : parse_levels(frame, "asks")) {
        tracked.apply_level(Side::Sell, l.price, l.quantity,
                            static_cast<std::uint32_t>(l.orders));
    }
}

void apply_delta(TrackedBook& tracked, const std::string& frame) {
    for (const ParsedLevel& l : parse_levels(frame, "levels")) {
        REQUIRE_FALSE(l.side.empty());
        tracked.apply_level(l.side == "BID" ? Side::Buy : Side::Sell, l.price,
                            l.quantity, static_cast<std::uint32_t>(l.orders));
    }
}

}  // namespace

// ---------------------------------------------------------------------------

TEST_CASE("snapshot plus replayed deltas equals the engine book", "[gateway]") {
    for_each_engine([](IBook& book, EngineVersion version) {
        SymbolStream stream("MBK");
        EventSink sink;

        // Seed some resting liquidity before anyone subscribes, so the snapshot
        // has real content rather than starting from an empty book — an
        // empty-book start would let a broken snapshot path pass.
        for (int i = 0; i < 8; ++i) {
            book.submit(limit(static_cast<ClientOrderId>(100 + i), Side::Buy,
                              100'000 - i * 10, 50 + i, 1), sink);
            book.submit(limit(static_cast<ClientOrderId>(200 + i), Side::Sell,
                              100'100 + i * 10, 40 + i, 2), sink);
        }
        sink.clear();

        const SubscriberPtr sub = stream.subscribe();
        SeqNum snap_seq = 0;
        const std::string snapshot = stream.build_snapshot(book, kTrackDepth, snap_seq);

        TrackedBook tracked;
        apply_snapshot(tracked, snapshot);

        {
            std::string detail;
            INFO("engine = " << to_string(version) << " (snapshot) " << detail);
            REQUIRE(tracked.equals(book, kTrackDepth, detail));
        }

        // A deterministic mixed workload: adds, crossing takes that consume
        // levels entirely, and cancels. Seeded arithmetic rather than a RNG so
        // a failure is reproducible from the test name alone.
        std::int64_t last_seq = static_cast<std::int64_t>(snap_seq);
        ClientOrderId next_id = 1000;

        for (int step = 0; step < 120; ++step) {
            sink.clear();

            const int mode = step % 5;
            if (mode == 0 || mode == 1) {
                const Side side  = (step % 2 == 0) ? Side::Buy : Side::Sell;
                const Price px   = (side == Side::Buy) ? 100'000 - (step % 7) * 10
                                                       : 100'100 + (step % 7) * 10;
                book.submit(limit(next_id++, side, px, 10 + (step % 13), 1), sink);
            } else if (mode == 2) {
                // Crossing order: takes liquidity and can empty a level, which
                // is the case that exercises quantity-0 removal deltas.
                book.submit(limit(next_id++, Side::Buy, 100'150, 60, 3), sink);
            } else if (mode == 3) {
                book.submit(limit(next_id++, Side::Sell, 99'950, 60, 4), sink);
            } else {
                // Cancel something that should still be resting.
                book.cancel(static_cast<ClientOrderId>(100 + (step % 8)), 1, sink);
            }

            const std::size_t before = sub->queue.size();
            stream.publish_delta(book, sink);

            for (const std::string& frame : drain(sub)) {
                const std::string body = frame;  // includes SSE framing
                REQUIRE(type_of(body) == "delta");

                const std::int64_t seq = seq_of(body);
                INFO("engine = " << to_string(version) << " step " << step);
                // Contiguous: a gap here would mean the client is entitled to
                // re-snapshot, and a client that re-snapshots constantly is a
                // gateway bug rather than a network condition.
                REQUIRE(seq == last_seq + 1);
                last_seq = seq;

                apply_delta(tracked, body);
            }
            (void)before;

            std::string detail;
            INFO("engine = " << to_string(version) << " step " << step
                             << " divergence: " << detail);
            REQUIRE(tracked.equals(book, kTrackDepth, detail));
        }
    });
}

TEST_CASE("a fully consumed level is removed by a quantity-zero delta",
          "[gateway]") {
    for_each_engine([](IBook& book, EngineVersion version) {
        SymbolStream stream("MBK");
        EventSink sink;

        book.submit(limit(1, Side::Sell, 100'100, 50, 1), sink);
        sink.clear();

        const SubscriberPtr sub = stream.subscribe();
        SeqNum seq = 0;
        stream.build_snapshot(book, kTrackDepth, seq);
        drain(sub);

        // Consume the entire level.
        book.submit(limit(2, Side::Buy, 100'100, 50, 2), sink);
        stream.publish_delta(book, sink);

        const std::vector<std::string> frames = drain(sub);
        REQUIRE(frames.size() == 1);

        const std::vector<ParsedLevel> levels = parse_levels(frames[0], "levels");
        INFO("engine = " << to_string(version));

        bool saw_removal = false;
        for (const ParsedLevel& l : levels) {
            if (l.side == "ASK" && l.price == 100'100) {
                CHECK(l.quantity == 0);
                CHECK(l.orders == 0);
                saw_removal = true;
            }
        }
        CHECK(saw_removal);

        // And the trade printed, at the resting order's price.
        const std::vector<ParsedLevel> trades = parse_trades(frames[0]);
        REQUIRE(trades.size() == 1);
        CHECK(trades[0].price == 100'100);
        CHECK(trades[0].quantity == 50);
        CHECK(trades[0].taker_side == "BUY");
    });
}

TEST_CASE("a rejected order produces no delta and does not advance the sequence",
          "[gateway]") {
    for_each_engine([](IBook& book, EngineVersion) {
        SymbolStream stream("MBK");
        EventSink sink;

        book.submit(limit(1, Side::Buy, 100'000, 10, 1), sink);
        sink.clear();

        const SubscriberPtr sub = stream.subscribe();
        SeqNum seq = 0;
        stream.build_snapshot(book, kTrackDepth, seq);
        drain(sub);

        const SeqNum before = stream.seq();

        // Off-tick against a tick size of 1 is hard to produce, so use a
        // POST_ONLY that would cross: rejected, book unchanged.
        book.submit(typed(2, Side::Sell, OrdType::PostOnly, 99'000, 10, 2), sink);
        stream.publish_delta(book, sink);

        CHECK(stream.seq() == before);
        CHECK(drain(sub).empty());
    });
}

TEST_CASE("a heartbeat repeats the sequence rather than advancing it",
          "[gateway]") {
    IBook* raw = nullptr;
    auto book = make_book(EngineVersion::V3_Tuned, default_config());
    raw = book.get();

    SymbolStream stream("MBK");
    EventSink sink;
    raw->submit(limit(1, Side::Buy, 100'000, 10, 1), sink);
    sink.clear();

    const SubscriberPtr sub = stream.subscribe();
    SeqNum seq = 0;
    stream.build_snapshot(*raw, kTrackDepth, seq);
    drain(sub);

    const SeqNum before = stream.seq();
    stream.publish_heartbeat();

    const std::vector<std::string> frames = drain(sub);
    REQUIRE(frames.size() == 1);
    CHECK(type_of(frames[0]) == "heartbeat");
    CHECK(seq_of(frames[0]) == static_cast<std::int64_t>(before));
    CHECK(stream.seq() == before);
}

TEST_CASE("a late subscriber receives a snapshot consistent with the live book",
          "[gateway]") {
    // The resnapshot path a client takes after detecting a gap. If this were
    // wrong, gap recovery would resynchronise onto an incorrect book and the
    // client would have no way to tell.
    for_each_engine([](IBook& book, EngineVersion version) {
        SymbolStream stream("MBK");
        EventSink sink;

        const SubscriberPtr early = stream.subscribe();
        SeqNum seq = 0;
        stream.build_snapshot(book, kTrackDepth, seq);
        drain(early);

        for (int i = 0; i < 20; ++i) {
            sink.clear();
            book.submit(limit(static_cast<ClientOrderId>(500 + i),
                              (i % 2 == 0) ? Side::Buy : Side::Sell,
                              (i % 2 == 0) ? 100'000 - i : 100'100 + i,
                              25 + i, 1), sink);
            stream.publish_delta(book, sink);
        }
        drain(early);

        SeqNum late_seq = 0;
        const std::string snapshot = stream.build_snapshot(book, kTrackDepth, late_seq);

        TrackedBook tracked;
        apply_snapshot(tracked, snapshot);

        std::string detail;
        INFO("engine = " << to_string(version) << " " << detail);
        REQUIRE(tracked.equals(book, kTrackDepth, detail));
        CHECK(late_seq == stream.seq());
    });
}
