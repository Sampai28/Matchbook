#include "server/gateway.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

#include "server/json.hpp"

namespace matchbook::gateway {

namespace {

// SSE framing: an event name, one data line, and a blank line to terminate.
//
// The payload is single-line JSON on purpose. SSE splits a multi-line `data:`
// field across several lines and the client rejoins them with newlines, which
// works but means a pretty-printed payload silently changes shape in transit.
std::string sse_frame(std::string_view event, std::string_view payload) {
    std::string out;
    out.reserve(payload.size() + event.size() + 24);
    out += "event: ";
    out += event;
    out += "\ndata: ";
    out += payload;
    out += "\n\n";
    return out;
}

void write_levels(json::Writer& w, std::string_view key,
                  const std::vector<LevelView>& levels) {
    w.key(key);
    w.begin_array();
    for (const LevelView& l : levels) {
        w.begin_object();
        w.field("price", static_cast<std::int64_t>(l.price));
        w.field("quantity", static_cast<std::int64_t>(l.quantity));
        w.field("orders", static_cast<std::uint64_t>(l.order_count));
        w.end_object();
    }
    w.end_array();
}

}  // namespace

// ---------------------------------------------------------------------------
// Subscriber
// ---------------------------------------------------------------------------

bool Subscriber::offer(const std::string& frame) {
    {
        std::lock_guard<std::mutex> lock(mu);
        if (closed) return false;
        if (queue.size() >= kSubscriberQueueLimit) {
            // Drop rather than block. The caller is the thread that just
            // mutated the book; blocking it here would let one slow browser
            // stall order entry for every other client. The dropped message
            // becomes a sequence gap, which the client is required to detect
            // and recover from by re-snapshotting.
            ++dropped;
            return false;
        }
        queue.push_back(frame);
    }
    cv.notify_one();
    return true;
}

void Subscriber::close() {
    {
        std::lock_guard<std::mutex> lock(mu);
        closed = true;
    }
    // Wake the streaming thread so it observes `closed` and returns, rather
    // than sitting in its condition-variable wait until the next heartbeat.
    cv.notify_all();
}

// ---------------------------------------------------------------------------
// SymbolStream
// ---------------------------------------------------------------------------

void SymbolStream::reindex(const IBook& book, std::size_t depth) {
    const BookSnapshot snap = book.snapshot(depth);
    bids_.clear();
    asks_.clear();
    for (const LevelView& l : snap.bids) {
        bids_[l.price] = LevelState{l.quantity, l.order_count};
    }
    for (const LevelView& l : snap.asks) {
        asks_[l.price] = LevelState{l.quantity, l.order_count};
    }
    indexed_ = true;
}

std::string SymbolStream::build_snapshot(const IBook& book, std::size_t depth,
                                         SeqNum& seq_out) {
    if (seq_ == 0) seq_ = 1;

    // Track the full window even when the client asked for a shallow view, so
    // that a later delta for a level below their depth is still diffed
    // correctly rather than appearing out of nowhere.
    reindex(book, kTrackDepth);

    const BookSnapshot snap = book.snapshot(depth);

    json::Writer w;
    w.begin_object();
    w.field("type", "snapshot");
    w.field("seq", static_cast<std::uint64_t>(seq_));
    w.field("symbol", symbol_);
    w.field("engine", to_string(book.version()));
    write_levels(w, "bids", snap.bids);
    write_levels(w, "asks", snap.asks);
    w.field("restingOrders", static_cast<std::uint64_t>(book.resting_count()));
    w.end_object();

    seq_out = seq_;
    return w.str();
}

std::size_t SymbolStream::publish_delta(const IBook& book, const EventSink& sink) {
    if (!indexed_) {
        // No subscriber has ever forced an index build. Establish the baseline
        // silently: there is nobody to send a delta to, and diffing against an
        // empty map would emit the entire book as "changed".
        reindex(book, kTrackDepth);
        return 0;
    }

    const BookSnapshot snap = book.snapshot(kTrackDepth);

    std::map<Price, LevelState, std::greater<Price>> new_bids;
    std::map<Price, LevelState>                      new_asks;
    for (const LevelView& l : snap.bids) new_bids[l.price] = LevelState{l.quantity, l.order_count};
    for (const LevelView& l : snap.asks) new_asks[l.price] = LevelState{l.quantity, l.order_count};

    struct Change {
        Side          side;
        Price         price;
        Qty           quantity;
        std::uint32_t orders;
    };
    std::vector<Change> changes;

    // Levels that appeared or changed.
    for (const auto& [price, state] : new_bids) {
        const auto it = bids_.find(price);
        if (it == bids_.end() || !(it->second == state)) {
            changes.push_back({Side::Buy, price, state.quantity, state.orders});
        }
    }
    for (const auto& [price, state] : new_asks) {
        const auto it = asks_.find(price);
        if (it == asks_.end() || !(it->second == state)) {
            changes.push_back({Side::Sell, price, state.quantity, state.orders});
        }
    }
    // Levels that vanished, emitted as quantity 0 so the client deletes them.
    // Without this a fully-consumed level would linger on every client forever,
    // and the ladder would show liquidity that does not exist.
    for (const auto& [price, state] : bids_) {
        if (new_bids.find(price) == new_bids.end()) {
            changes.push_back({Side::Buy, price, 0, 0});
        }
    }
    for (const auto& [price, state] : asks_) {
        if (new_asks.find(price) == new_asks.end()) {
            changes.push_back({Side::Sell, price, 0, 0});
        }
    }

    std::vector<const Event*> trades;
    for (const Event& e : sink.events()) {
        if (e.kind == EventKind::Fill) trades.push_back(&e);
    }

    bids_ = std::move(new_bids);
    asks_ = std::move(new_asks);

    if (changes.empty() && trades.empty()) {
        // Nothing observable happened — a rejected order, for instance. Not a
        // message, and specifically not a sequence increment: a client that
        // saw seq jump for a no-op would have no way to tell it apart from a
        // delta it failed to receive.
        return 0;
    }

    ++seq_;

    json::Writer w;
    w.begin_object();
    w.field("type", "delta");
    w.field("seq", static_cast<std::uint64_t>(seq_));
    w.field("symbol", symbol_);

    w.key("levels");
    w.begin_array();
    for (const Change& c : changes) {
        w.begin_object();
        w.field("side", c.side == Side::Buy ? "BID" : "ASK");
        w.field("price", static_cast<std::int64_t>(c.price));
        w.field("quantity", static_cast<std::int64_t>(c.quantity));
        w.field("orders", static_cast<std::uint64_t>(c.orders));
        w.end_object();
    }
    w.end_array();

    w.key("trades");
    w.begin_array();
    for (const Event* e : trades) {
        w.begin_object();
        w.field("price", static_cast<std::int64_t>(e->price));
        w.field("quantity", static_cast<std::int64_t>(e->quantity));
        w.field("takerSide", to_string(e->side));
        w.end_object();
    }
    w.end_array();

    w.end_object();

    const std::string frame = sse_frame("delta", w.str());
    fan_out(frame);

    std::lock_guard<std::mutex> lock(mu_);
    return subscribers_.size();
}

void SymbolStream::publish_heartbeat() {
    json::Writer w;
    w.begin_object();
    w.field("type", "heartbeat");
    // Repeats the current seq rather than advancing it: a heartbeat carries no
    // state change, so advancing would make the client think it missed one.
    w.field("seq", static_cast<std::uint64_t>(seq_));
    w.field("symbol", symbol_);
    w.end_object();
    fan_out(sse_frame("heartbeat", w.str()));
}

void SymbolStream::fan_out(const std::string& frame) {
    std::vector<SubscriberPtr> targets;
    {
        std::lock_guard<std::mutex> lock(mu_);
        targets = subscribers_;
        ++messages_sent_;
    }
    // Copied out before offering so the gateway lock is not held across
    // subscriber locks — that nesting is the shape a deadlock grows from once
    // unsubscribe starts taking them in the other order.
    std::uint64_t dropped_here = 0;
    for (const SubscriberPtr& sub : targets) {
        if (!sub->offer(frame)) ++dropped_here;
    }
    if (dropped_here != 0) {
        std::lock_guard<std::mutex> lock(mu_);
        dropped_ += dropped_here;
    }
}

SubscriberPtr SymbolStream::subscribe() {
    auto sub = std::make_shared<Subscriber>();
    std::lock_guard<std::mutex> lock(mu_);
    subscribers_.push_back(sub);
    return sub;
}

void SymbolStream::unsubscribe(const SubscriberPtr& sub) {
    std::lock_guard<std::mutex> lock(mu_);
    subscribers_.erase(
        std::remove(subscribers_.begin(), subscribers_.end(), sub),
        subscribers_.end());
}

std::size_t SymbolStream::subscriber_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return subscribers_.size();
}

// ---------------------------------------------------------------------------
// Gateway
// ---------------------------------------------------------------------------

SymbolStream* Gateway::stream(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = streams_.find(symbol);
    return it == streams_.end() ? nullptr : it->second.get();
}

SymbolStream& Gateway::ensure(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = streams_.find(symbol);
    if (it == streams_.end()) {
        it = streams_.emplace(symbol, std::make_unique<SymbolStream>(symbol)).first;
    }
    return *it->second;
}

std::size_t Gateway::total_subscribers() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::size_t n = 0;
    for (const auto& [name, s] : streams_) n += s->subscriber_count();
    return n;
}

std::uint64_t Gateway::total_messages() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::uint64_t n = 0;
    for (const auto& [name, s] : streams_) n += s->messages_sent();
    return n;
}

std::uint64_t Gateway::total_dropped() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::uint64_t n = 0;
    for (const auto& [name, s] : streams_) n += s->dropped();
    return n;
}

void Gateway::heartbeat_all() {
    std::vector<SymbolStream*> all;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& [name, s] : streams_) all.push_back(s.get());
    }
    for (SymbolStream* s : all) s->publish_heartbeat();
}

// ---------------------------------------------------------------------------
// TrackedBook — the client-side model, in C++, for the correctness test
// ---------------------------------------------------------------------------

void TrackedBook::apply_level(Side side, Price price, Qty quantity,
                              std::uint32_t orders) {
    if (side == Side::Buy) {
        if (quantity == 0) {
            bids.erase(price);
        } else {
            bids[price] = LevelState{quantity, orders};
        }
    } else {
        if (quantity == 0) {
            asks.erase(price);
        } else {
            asks[price] = LevelState{quantity, orders};
        }
    }
}

bool TrackedBook::equals(const IBook& book, std::size_t depth,
                         std::string& detail) const {
    const BookSnapshot snap = book.snapshot(depth);

    auto compare = [&detail](std::string_view side,
                             const std::vector<LevelView>& actual,
                             const std::vector<std::pair<Price, LevelState>>& mine) {
        if (actual.size() != mine.size()) {
            std::ostringstream os;
            os << side << ": tracked " << mine.size() << " levels, engine has "
               << actual.size();
            detail += os.str();
            return false;
        }
        for (std::size_t i = 0; i < actual.size(); ++i) {
            if (actual[i].price != mine[i].first ||
                actual[i].quantity != mine[i].second.quantity ||
                actual[i].order_count != mine[i].second.orders) {
                std::ostringstream os;
                os << side << " level " << i << ": engine price="
                   << actual[i].price << " qty=" << actual[i].quantity
                   << " orders=" << actual[i].order_count
                   << " vs tracked price=" << mine[i].first
                   << " qty=" << mine[i].second.quantity
                   << " orders=" << mine[i].second.orders;
                detail += os.str();
                return false;
            }
        }
        return true;
    };

    std::vector<std::pair<Price, LevelState>> my_bids(bids.begin(), bids.end());
    std::vector<std::pair<Price, LevelState>> my_asks(asks.begin(), asks.end());
    if (my_bids.size() > depth) my_bids.resize(depth);
    if (my_asks.size() > depth) my_asks.resize(depth);

    return compare("bids", snap.bids, my_bids) && compare("asks", snap.asks, my_asks);
}

}  // namespace matchbook::gateway
