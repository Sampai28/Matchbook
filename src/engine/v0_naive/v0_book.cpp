// V0 — naive baseline implementation.
//
// Data structures, chosen for readability:
//   bids_ : std::map<Price, std::list<Order>>  — ordered, best bid is rbegin()
//   asks_ : std::map<Price, std::list<Order>>  — ordered, best ask is begin()
//   index_: std::unordered_map<ClientOrderId, Locator>
//
// Every enqueue allocates: std::map allocates a red-black tree node per new
// price level, and std::list allocates a node per order. That is the cost V1
// exists to remove.
//
// The matching semantics implemented here are the specification. V1-V3 must
// reproduce this event stream byte for byte, and tools/reference_matcher.py
// implements the same rules independently in Python.

#include "v0_book.hpp"

#include <algorithm>
#include <cstdio>
#include <list>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>

namespace matchbook::v0 {
namespace {

class V0Book final : public IBook {
public:
    explicit V0Book(const BookConfig& cfg) : cfg_(cfg) {}

    SubmitResult submit(const NewOrder& req, EventSink& sink) override;
    CancelResult cancel(ClientOrderId id, ParticipantId participant,
                        EventSink& sink) override;
    ReplaceResult replace(const ReplaceRequest& req, EventSink& sink) override;

    [[nodiscard]] BookSnapshot snapshot(std::size_t depth) const override;
    [[nodiscard]] bool best_bid(Price& out) const override;
    [[nodiscard]] bool best_ask(Price& out) const override;
    [[nodiscard]] std::size_t resting_count() const override { return index_.size(); }
    [[nodiscard]] const Stats& stats() const override { return stats_; }
    [[nodiscard]] const BookConfig& config() const override { return cfg_; }
    [[nodiscard]] EngineVersion version() const override { return EngineVersion::V0_Naive; }
    [[nodiscard]] bool check_invariants(std::string& detail) const override;
    [[nodiscard]] std::string debug_dump() const override;

private:
    using OrderList = std::list<Order>;
    using LevelMap  = std::map<Price, OrderList>;

    // Where a resting order lives. The list iterator stays valid across
    // insertions and unrelated erasures, which is the only reason a naive
    // implementation can cancel in better than O(n).
    struct Locator {
        Side                 side;
        Price                price;
        OrderList::iterator  it;
    };

    LevelMap&       side_map(Side s) noexcept { return s == Side::Buy ? bids_ : asks_; }
    const LevelMap& side_map(Side s) const noexcept { return s == Side::Buy ? bids_ : asks_; }

    // Does an incoming order at `price` cross the resting order at `resting`?
    static bool crosses(Side taker_side, Price taker_price, Price resting) noexcept {
        if (taker_price == kNoPrice) return true;  // MARKET crosses everything
        return taker_side == Side::Buy ? taker_price >= resting : taker_price <= resting;
    }

    // Quantity available to a taker at prices that cross. Used by FOK, which
    // must decide before emitting any event at all.
    [[nodiscard]] Qty available_to(Side taker_side, Price limit,
                                   ParticipantId participant) const;

    void emit_ack(EventSink& sink, ClientOrderId cid, OrderStatus st);
    void emit_reject(EventSink& sink, ClientOrderId cid, RejectReason r);
    void emit_cancel(EventSink& sink, ClientOrderId cid);
    void emit_fill(EventSink& sink, ClientOrderId taker, ClientOrderId maker,
                   Price px, Qty qty, Side taker_side);

    Qty match(const NewOrder& req, Order& taker, EventSink& sink);
    void rest_order(const NewOrder& req, Order& taker, EventSink& sink);
    void remove_resting(const Locator& loc, ClientOrderId cid);

    BookConfig cfg_;
    LevelMap   bids_;
    LevelMap   asks_;
    std::unordered_map<ClientOrderId, Locator> index_;
    Stats      stats_;
    SeqNum     seq_      = 0;
    OrderId    next_id_  = 1;
};

// ---------------------------------------------------------------------------
// event emission
// ---------------------------------------------------------------------------

void V0Book::emit_ack(EventSink& sink, ClientOrderId cid, OrderStatus st) {
    Event e;
    e.kind      = EventKind::Ack;
    e.seq       = ++seq_;
    e.client_id = cid;
    e.status    = st;
    sink.push(e);
}

void V0Book::emit_reject(EventSink& sink, ClientOrderId cid, RejectReason r) {
    Event e;
    e.kind      = EventKind::Reject;
    e.seq       = ++seq_;
    e.client_id = cid;
    e.reason    = r;
    e.status    = OrderStatus::Rejected;
    sink.push(e);
    stats_.count_reject(r);
}

void V0Book::emit_cancel(EventSink& sink, ClientOrderId cid) {
    Event e;
    e.kind      = EventKind::Cancel;
    e.seq       = ++seq_;
    e.client_id = cid;
    e.status    = OrderStatus::Cancelled;
    sink.push(e);
}

void V0Book::emit_fill(EventSink& sink, ClientOrderId taker, ClientOrderId maker,
                       Price px, Qty qty, Side taker_side) {
    Event e;
    e.kind            = EventKind::Fill;
    e.seq             = ++seq_;
    e.client_id       = taker;
    e.maker_client_id = maker;
    e.price           = px;
    e.quantity        = qty;
    e.side            = taker_side;
    sink.push(e);

    stats_.fills++;
    // Both sides accumulate the same amount on every fill. The conservation
    // invariant asserts they stay equal; a divergence means the matching loop
    // dropped or double-counted a leg.
    stats_.buy_filled_qty  += static_cast<std::uint64_t>(qty);
    stats_.sell_filled_qty += static_cast<std::uint64_t>(qty);
    stats_.buy_notional    += px * qty;
    stats_.sell_notional   += px * qty;
}

// ---------------------------------------------------------------------------
// liquidity inspection
// ---------------------------------------------------------------------------

Qty V0Book::available_to(Side taker_side, Price limit, ParticipantId participant) const {
    const LevelMap& opp = side_map(opposite(taker_side));
    Qty total = 0;

    // Buyers consume asks from the lowest price up; sellers consume bids from
    // the highest price down.
    if (taker_side == Side::Buy) {
        for (auto it = opp.begin(); it != opp.end(); ++it) {
            if (!crosses(taker_side, limit, it->first)) break;
            for (const Order& o : it->second) {
                // Self-trade prevention cancels the resting order rather than
                // filling against it, so its quantity is not available.
                if (o.participant == participant) continue;
                total += o.remaining();
            }
        }
    } else {
        for (auto it = opp.rbegin(); it != opp.rend(); ++it) {
            if (!crosses(taker_side, limit, it->first)) break;
            for (const Order& o : it->second) {
                if (o.participant == participant) continue;
                total += o.remaining();
            }
        }
    }
    return total;
}

// ---------------------------------------------------------------------------
// matching
// ---------------------------------------------------------------------------

// Consumes liquidity from the opposite side while the taker's price crosses.
// Returns the quantity filled. Emits one Fill per maker touched, plus a Cancel
// for any resting order removed by self-trade prevention.
Qty V0Book::match(const NewOrder& req, Order& taker, EventSink& sink) {
    LevelMap& opp = side_map(opposite(req.side));
    Qty filled = 0;

    while (taker.remaining() > 0 && !opp.empty()) {
        // Best opposite level: lowest ask for a buyer, highest bid for a seller.
        auto level_it = (req.side == Side::Buy) ? opp.begin() : std::prev(opp.end());
        const Price level_price = level_it->first;

        if (!crosses(req.side, req.price, level_price)) break;

        OrderList& queue = level_it->second;

        // FIFO within a level: front of the list is the oldest order, which is
        // the "time" half of price-time priority.
        while (taker.remaining() > 0 && !queue.empty()) {
            Order& maker = queue.front();

            if (maker.participant == req.participant) {
                // Self-trade prevention: cancel the resting order and keep
                // going. The aggressor is not penalised for the collision.
                const ClientOrderId maker_cid = maker.client_id;
                maker.status = OrderStatus::Cancelled;
                index_.erase(maker_cid);
                queue.pop_front();
                emit_cancel(sink, maker_cid);
                stats_.rejects[static_cast<std::size_t>(RejectReason::SelfTradePrevented)]++;
                continue;
            }

            const Qty trade_qty = std::min(taker.remaining(), maker.remaining());
            // The trade prints at the resting order's price. The maker set the
            // terms by arriving first; the taker accepted them.
            const Price trade_px = maker.price;

            maker.filled += trade_qty;
            taker.filled += trade_qty;
            filled       += trade_qty;

            emit_fill(sink, req.client_id, maker.client_id, trade_px, trade_qty, req.side);

            if (maker.remaining() == 0) {
                const ClientOrderId maker_cid = maker.client_id;
                maker.status = OrderStatus::Filled;
                index_.erase(maker_cid);
                queue.pop_front();
            } else {
                maker.status = OrderStatus::PartiallyFilled;
            }
        }

        if (queue.empty()) opp.erase(level_it);
    }

    return filled;
}

void V0Book::rest_order(const NewOrder& req, Order& taker, EventSink& sink) {
    LevelMap& own = side_map(req.side);
    OrderList& queue = own[req.price];  // default-constructs the level if absent

    taker.status = (taker.filled > 0) ? OrderStatus::PartiallyFilled : OrderStatus::New;
    queue.push_back(taker);

    Locator loc{req.side, req.price, std::prev(queue.end())};
    index_.emplace(req.client_id, loc);

    stats_.orders_resting = index_.size();
    emit_ack(sink, req.client_id, taker.status);
}

void V0Book::remove_resting(const Locator& loc, ClientOrderId cid) {
    LevelMap& m = side_map(loc.side);
    auto level_it = m.find(loc.price);
    if (level_it == m.end()) return;
    level_it->second.erase(loc.it);
    if (level_it->second.empty()) m.erase(level_it);
    index_.erase(cid);
    stats_.orders_resting = index_.size();
}

// ---------------------------------------------------------------------------
// public operations
// ---------------------------------------------------------------------------

SubmitResult V0Book::submit(const NewOrder& req, EventSink& sink) {
    stats_.orders_received++;
    SubmitResult res;

    // The book owns the client-order-id index, so the duplicate check lives
    // here rather than in the stateless validator.
    if (index_.find(req.client_id) != index_.end()) {
        res.reason = RejectReason::DuplicateClientOrderId;
        emit_reject(sink, req.client_id, res.reason);
        return res;
    }

    // POST_ONLY must not take liquidity. Checked before any state changes so a
    // rejection leaves the book untouched.
    if (req.type == OrdType::PostOnly) {
        const LevelMap& opp = side_map(opposite(req.side));
        if (!opp.empty()) {
            const Price best = (req.side == Side::Buy) ? opp.begin()->first
                                                       : std::prev(opp.end())->first;
            if (crosses(req.side, req.price, best)) {
                res.reason = RejectReason::PostOnlyWouldCross;
                emit_reject(sink, req.client_id, res.reason);
                return res;
            }
        }
    }

    // FOK is all-or-nothing: decide before emitting anything, because a partial
    // fill followed by a rejection is not a thing that can be undone.
    if (req.type == OrdType::FOK) {
        if (available_to(req.side, req.price, req.participant) < req.quantity) {
            res.reason = RejectReason::FokUnfillable;
            emit_reject(sink, req.client_id, res.reason);
            return res;
        }
    }

    Order taker;
    taker.client_id   = req.client_id;
    taker.id          = next_id_++;
    taker.participant = req.participant;
    taker.price       = req.price;
    taker.quantity    = req.quantity;
    taker.side        = req.side;
    taker.type        = req.type;
    taker.seq         = ++seq_;

    const bool takes_liquidity = (req.type != OrdType::PostOnly);
    const Qty filled = takes_liquidity ? match(req, taker, sink) : 0;

    res.accepted   = true;
    res.order_id   = taker.id;
    res.filled_qty = filled;
    res.remaining  = taker.remaining();
    stats_.orders_accepted++;

    if (taker.remaining() == 0) {
        taker.status = OrderStatus::Filled;
        res.status   = OrderStatus::Filled;
        emit_ack(sink, req.client_id, OrderStatus::Filled);
        return res;
    }

    switch (req.type) {
        case OrdType::Limit:
        case OrdType::PostOnly:
            rest_order(req, taker, sink);
            res.resting = true;
            res.status  = taker.status;
            break;

        case OrdType::Market:
        case OrdType::IOC:
            // Nothing left to do with the remainder: it leaves.
            res.status = OrderStatus::Cancelled;
            if (filled == 0) {
                stats_.rejects[static_cast<std::size_t>(RejectReason::NoLiquidity)]++;
            }
            emit_cancel(sink, req.client_id);
            break;

        case OrdType::FOK:
            // Unreachable: availability was checked above, so a FOK that got
            // here filled completely. Handled defensively rather than asserted,
            // because release builds must never assert.
            res.status = OrderStatus::Cancelled;
            emit_cancel(sink, req.client_id);
            break;
    }

    return res;
}

CancelResult V0Book::cancel(ClientOrderId id, ParticipantId participant, EventSink& sink) {
    stats_.cancels_received++;
    CancelResult res;

    auto it = index_.find(id);
    if (it == index_.end()) {
        res.reason = RejectReason::UnknownOrder;
        emit_reject(sink, id, res.reason);
        return res;
    }

    Order& o = *it->second.it;
    if (o.participant != participant) {
        // Reported as unknown rather than "not yours": telling a participant
        // that someone else's order exists is an information leak.
        res.reason = RejectReason::UnknownOrder;
        emit_reject(sink, id, res.reason);
        return res;
    }
    if (is_terminal(o.status)) {
        res.reason = RejectReason::OrderAlreadyTerminal;
        emit_reject(sink, id, res.reason);
        return res;
    }

    res.cancelled_qty = o.remaining();
    o.status = OrderStatus::Cancelled;
    remove_resting(it->second, id);

    res.accepted = true;
    stats_.cancels_accepted++;
    emit_cancel(sink, id);
    return res;
}

ReplaceResult V0Book::replace(const ReplaceRequest& req, EventSink& sink) {
    stats_.replaces_received++;
    ReplaceResult res;

    auto it = index_.find(req.original_client_id);
    if (it == index_.end()) {
        res.reason = RejectReason::UnknownOrder;
        emit_reject(sink, req.original_client_id, res.reason);
        return res;
    }

    Locator loc = it->second;
    Order&  o   = *loc.it;

    if (o.participant != req.participant) {
        res.reason = RejectReason::UnknownOrder;
        emit_reject(sink, req.original_client_id, res.reason);
        return res;
    }
    if (is_terminal(o.status)) {
        res.reason = RejectReason::OrderAlreadyTerminal;
        emit_reject(sink, req.original_client_id, res.reason);
        return res;
    }

    const Side  side       = o.side;
    const OrdType type     = o.type;
    const Qty   already    = o.filled;
    const Price old_price  = o.price;

    // A pure quantity reduction at the same price keeps time priority. This is
    // the near-universal convention: shrinking an order takes nothing from
    // anyone queued behind it, so there is no reason to send it to the back.
    const bool price_unchanged = (req.new_price == kNoPrice || req.new_price == old_price);
    if (price_unchanged && req.new_quantity > already && req.new_quantity <= o.quantity) {
        o.quantity = req.new_quantity;
        res.accepted      = true;
        res.priority_kept = true;
        res.remaining     = o.remaining();
        res.new_order_id  = o.id;
        stats_.replaces_accepted++;
        emit_ack(sink, req.original_client_id, o.status);
        return res;
    }

    // Everything else is cancel-then-new, which loses priority.
    o.status = OrderStatus::Cancelled;
    remove_resting(loc, req.original_client_id);
    emit_cancel(sink, req.original_client_id);

    NewOrder fresh;
    fresh.client_id   = req.new_client_id;
    fresh.participant = req.participant;
    fresh.side        = side;
    fresh.type        = type;
    fresh.price       = (req.new_price == kNoPrice) ? old_price : req.new_price;
    fresh.quantity    = req.new_quantity;

    const SubmitResult sr = submit(fresh, sink);
    res.accepted     = sr.accepted;
    res.reason       = sr.reason;
    res.new_order_id = sr.order_id;
    res.filled_qty   = sr.filled_qty;
    res.remaining    = sr.remaining;
    res.resting      = sr.resting;
    if (sr.accepted) stats_.replaces_accepted++;
    return res;
}

// ---------------------------------------------------------------------------
// queries
// ---------------------------------------------------------------------------

bool V0Book::best_bid(Price& out) const {
    if (bids_.empty()) return false;
    out = std::prev(bids_.end())->first;
    return true;
}

bool V0Book::best_ask(Price& out) const {
    if (asks_.empty()) return false;
    out = asks_.begin()->first;
    return true;
}

BookSnapshot V0Book::snapshot(std::size_t depth) const {
    BookSnapshot snap;
    snap.seq = seq_;

    std::size_t n = 0;
    for (auto it = bids_.rbegin(); it != bids_.rend() && n < depth; ++it, ++n) {
        LevelView lv;
        lv.price = it->first;
        for (const Order& o : it->second) lv.quantity += o.remaining();
        lv.order_count = static_cast<std::uint32_t>(it->second.size());
        snap.bids.push_back(lv);
    }

    n = 0;
    for (auto it = asks_.begin(); it != asks_.end() && n < depth; ++it, ++n) {
        LevelView lv;
        lv.price = it->first;
        for (const Order& o : it->second) lv.quantity += o.remaining();
        lv.order_count = static_cast<std::uint32_t>(it->second.size());
        snap.asks.push_back(lv);
    }
    return snap;
}

bool V0Book::check_invariants(std::string& detail) const {
    // 1. The book is never crossed.
    if (!bids_.empty() && !asks_.empty()) {
        const Price bb = std::prev(bids_.end())->first;
        const Price ba = asks_.begin()->first;
        if (bb >= ba) {
            detail += "crossed book: best_bid=" + std::to_string(bb) +
                      " >= best_ask=" + std::to_string(ba) + "\n";
            return false;
        }
    }

    // 2. Total resting orders equals the index size.
    std::size_t counted = 0;
    for (const auto& [px, q] : bids_) counted += q.size();
    for (const auto& [px, q] : asks_) counted += q.size();
    if (counted != index_.size()) {
        detail += "resting count " + std::to_string(counted) +
                  " != index size " + std::to_string(index_.size()) + "\n";
        return false;
    }

    // 3. No empty levels linger, and every resting order has quantity left.
    for (const auto& [px, q] : bids_) {
        if (q.empty()) {
            detail += "empty bid level at " + std::to_string(px) + "\n";
            return false;
        }
        for (const Order& o : q) {
            if (o.remaining() <= 0) {
                detail += "bid order " + std::to_string(o.client_id) +
                          " rests with no quantity remaining\n";
                return false;
            }
        }
    }
    for (const auto& [px, q] : asks_) {
        if (q.empty()) {
            detail += "empty ask level at " + std::to_string(px) + "\n";
            return false;
        }
        for (const Order& o : q) {
            if (o.remaining() <= 0) {
                detail += "ask order " + std::to_string(o.client_id) +
                          " rests with no quantity remaining\n";
                return false;
            }
        }
    }

    // 4. Conservation: every fill added the same amount to both sides.
    if (stats_.buy_filled_qty != stats_.sell_filled_qty) {
        detail += "conservation broken: buy_filled=" +
                  std::to_string(stats_.buy_filled_qty) + " sell_filled=" +
                  std::to_string(stats_.sell_filled_qty) + "\n";
        return false;
    }
    if (stats_.buy_notional != stats_.sell_notional) {
        detail += "notional reconciliation broken: buy=" +
                  std::to_string(stats_.buy_notional) + " sell=" +
                  std::to_string(stats_.sell_notional) + "\n";
        return false;
    }

    return true;
}

std::string V0Book::debug_dump() const {
    std::ostringstream os;
    os << "=== V0 book dump: " << cfg_.symbol << " ===\n";
    os << "seq=" << seq_ << " resting=" << index_.size()
       << " fills=" << stats_.fills << "\n";

    os << "-- asks (ascending) --\n";
    for (const auto& [px, q] : asks_) {
        Qty total = 0;
        for (const Order& o : q) total += o.remaining();
        os << "  " << px << "  qty=" << total << "  orders=" << q.size() << "\n";
    }
    os << "-- bids (descending) --\n";
    for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
        Qty total = 0;
        for (const Order& o : it->second) total += o.remaining();
        os << "  " << it->first << "  qty=" << total
           << "  orders=" << it->second.size() << "\n";
    }
    return os.str();
}

}  // namespace

std::unique_ptr<IBook> make(const BookConfig& cfg) {
    return std::make_unique<V0Book>(cfg);
}

}  // namespace matchbook::v0
