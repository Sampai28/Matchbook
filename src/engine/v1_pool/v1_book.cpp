// V1 — pooled orders and intrusive price levels.
//
// What changed from V0, and nothing else:
//   * Orders come from detail::OrderPool instead of being copied into a
//     std::list. Enqueueing allocates nothing.
//   * A price level is a detail::IntrusiveList rather than a std::list, so the
//     links live inside the Order and cancelling needs no iterator.
//   * The order index maps client id -> Order*, not client id -> iterator.
//   * Level aggregate quantity is maintained incrementally, so depth queries
//     stop walking every order at a level.
//
// What did NOT change: the price ladder is still std::map, so finding a price
// level is still a red-black tree descent with a cache miss at most nodes. That
// is V2's problem.
//
// The matching rules are identical to V0 line for line. Any divergence in the
// emitted event stream is a bug in this file, by definition.

#include "v1_book.hpp"

#include <algorithm>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>

#include "../common/order_pool.hpp"

namespace matchbook::v1 {
namespace {

using detail::IntrusiveList;
using detail::OrderPool;

class V1Book final : public IBook {
public:
    explicit V1Book(const BookConfig& cfg)
        : cfg_(cfg), pool_(cfg.max_orders) {
        index_.reserve(cfg.max_orders / 4);
    }

    SubmitResult  submit(const NewOrder& req, EventSink& sink) override;
    CancelResult  cancel(ClientOrderId id, ParticipantId participant, EventSink& sink) override;
    ReplaceResult replace(const ReplaceRequest& req, EventSink& sink) override;

    [[nodiscard]] BookSnapshot snapshot(std::size_t depth) const override;
    [[nodiscard]] bool best_bid(Price& out) const override;
    [[nodiscard]] bool best_ask(Price& out) const override;
    [[nodiscard]] std::size_t resting_count() const override { return index_.size(); }
    [[nodiscard]] const Stats& stats() const override { return stats_; }
    [[nodiscard]] const BookConfig& config() const override { return cfg_; }
    [[nodiscard]] EngineVersion version() const override { return EngineVersion::V1_Pool; }
    [[nodiscard]] bool check_invariants(std::string& detail) const override;
    [[nodiscard]] std::string debug_dump() const override;

private:
    using LevelMap = std::map<Price, IntrusiveList>;

    LevelMap&       side_map(Side s) noexcept { return s == Side::Buy ? bids_ : asks_; }
    const LevelMap& side_map(Side s) const noexcept { return s == Side::Buy ? bids_ : asks_; }

    static bool crosses(Side taker_side, Price taker_price, Price resting) noexcept {
        if (taker_price == kNoPrice) return true;
        return taker_side == Side::Buy ? taker_price >= resting : taker_price <= resting;
    }

    [[nodiscard]] Qty available_to(Side taker_side, Price limit, ParticipantId participant) const;

    void emit_ack(EventSink& sink, ClientOrderId cid, OrderStatus st);
    void emit_reject(EventSink& sink, ClientOrderId cid, RejectReason r);
    void emit_cancel(EventSink& sink, ClientOrderId cid);
    void emit_fill(EventSink& sink, ClientOrderId taker, ClientOrderId maker,
                   Price px, Qty qty, Side taker_side);

    Qty  match(const NewOrder& req, Order& taker, EventSink& sink);
    bool rest_order(const NewOrder& req, const Order& taker, EventSink& sink);
    void remove_resting(Order* o);

    BookConfig cfg_;
    OrderPool  pool_;
    LevelMap   bids_;
    LevelMap   asks_;
    std::unordered_map<ClientOrderId, Order*> index_;
    Stats      stats_;
    SeqNum     seq_     = 0;
    OrderId    next_id_ = 1;
};

// ---------------------------------------------------------------------------
// events
// ---------------------------------------------------------------------------

void V1Book::emit_ack(EventSink& sink, ClientOrderId cid, OrderStatus st) {
    Event e;
    e.kind = EventKind::Ack;
    e.seq = ++seq_;
    e.client_id = cid;
    e.status = st;
    sink.push(e);
}

void V1Book::emit_reject(EventSink& sink, ClientOrderId cid, RejectReason r) {
    Event e;
    e.kind = EventKind::Reject;
    e.seq = ++seq_;
    e.client_id = cid;
    e.reason = r;
    e.status = OrderStatus::Rejected;
    sink.push(e);
    stats_.count_reject(r);
}

void V1Book::emit_cancel(EventSink& sink, ClientOrderId cid) {
    Event e;
    e.kind = EventKind::Cancel;
    e.seq = ++seq_;
    e.client_id = cid;
    e.status = OrderStatus::Cancelled;
    sink.push(e);
}

void V1Book::emit_fill(EventSink& sink, ClientOrderId taker, ClientOrderId maker,
                       Price px, Qty qty, Side taker_side) {
    Event e;
    e.kind = EventKind::Fill;
    e.seq = ++seq_;
    e.client_id = taker;
    e.maker_client_id = maker;
    e.price = px;
    e.quantity = qty;
    e.side = taker_side;
    sink.push(e);

    stats_.fills++;
    stats_.buy_filled_qty  += static_cast<std::uint64_t>(qty);
    stats_.sell_filled_qty += static_cast<std::uint64_t>(qty);
    stats_.buy_notional    += px * qty;
    stats_.sell_notional   += px * qty;
}

// ---------------------------------------------------------------------------
// liquidity
// ---------------------------------------------------------------------------

Qty V1Book::available_to(Side taker_side, Price limit, ParticipantId participant) const {
    const LevelMap& opp = side_map(opposite(taker_side));
    Qty total = 0;

    auto scan_level = [&](const IntrusiveList& lvl) {
        for (const Order* o = lvl.head; o != nullptr; o = o->next) {
            if (o->participant == participant) continue;
            total += o->remaining();
        }
    };

    if (taker_side == Side::Buy) {
        for (auto it = opp.begin(); it != opp.end(); ++it) {
            if (!crosses(taker_side, limit, it->first)) break;
            scan_level(it->second);
        }
    } else {
        for (auto it = opp.rbegin(); it != opp.rend(); ++it) {
            if (!crosses(taker_side, limit, it->first)) break;
            scan_level(it->second);
        }
    }
    return total;
}

// ---------------------------------------------------------------------------
// matching
// ---------------------------------------------------------------------------

Qty V1Book::match(const NewOrder& req, Order& taker, EventSink& sink) {
    LevelMap& opp = side_map(opposite(req.side));
    Qty filled = 0;

    while (taker.remaining() > 0 && !opp.empty()) {
        auto level_it = (req.side == Side::Buy) ? opp.begin() : std::prev(opp.end());
        const Price level_price = level_it->first;
        if (!crosses(req.side, req.price, level_price)) break;

        IntrusiveList& queue = level_it->second;

        while (taker.remaining() > 0 && !queue.empty()) {
            Order* maker = queue.front();

            if (maker->participant == req.participant) {
                const ClientOrderId maker_cid = maker->client_id;
                queue.unlink(maker);
                index_.erase(maker_cid);
                maker->status = OrderStatus::Cancelled;
                pool_.release(maker);
                emit_cancel(sink, maker_cid);
                stats_.rejects[static_cast<std::size_t>(RejectReason::SelfTradePrevented)]++;
                continue;
            }

            const Qty   trade_qty = std::min(taker.remaining(), maker->remaining());
            const Price trade_px  = maker->price;

            maker->filled += trade_qty;
            taker.filled  += trade_qty;
            filled        += trade_qty;
            // The order stays linked but the level's aggregate must shrink now,
            // not when the order eventually leaves.
            queue.reduce(trade_qty);

            emit_fill(sink, req.client_id, maker->client_id, trade_px, trade_qty, req.side);

            if (maker->remaining() == 0) {
                const ClientOrderId maker_cid = maker->client_id;
                queue.unlink(maker);
                index_.erase(maker_cid);
                maker->status = OrderStatus::Filled;
                pool_.release(maker);
            } else {
                maker->status = OrderStatus::PartiallyFilled;
            }
        }

        if (queue.empty()) opp.erase(level_it);
    }

    return filled;
}

bool V1Book::rest_order(const NewOrder& req, const Order& taker, EventSink& sink) {
    Order* o = pool_.acquire();
    if (o == nullptr) {
        emit_reject(sink, req.client_id, RejectReason::BookFull);
        return false;
    }

    const std::int32_t slot = o->pool_slot;
    *o = taker;             // copy the fully-populated taker record
    o->pool_slot = slot;    // ...but keep this record's own slot
    o->status = (o->filled > 0) ? OrderStatus::PartiallyFilled : OrderStatus::New;

    side_map(req.side)[req.price].push_back(o);
    index_.emplace(req.client_id, o);
    stats_.orders_resting = index_.size();

    emit_ack(sink, req.client_id, o->status);
    return true;
}

void V1Book::remove_resting(Order* o) {
    LevelMap& m = side_map(o->side);
    auto level_it = m.find(o->price);
    if (level_it == m.end()) return;
    level_it->second.unlink(o);
    if (level_it->second.empty()) m.erase(level_it);
    index_.erase(o->client_id);
    pool_.release(o);
    stats_.orders_resting = index_.size();
}

// ---------------------------------------------------------------------------
// operations
// ---------------------------------------------------------------------------

SubmitResult V1Book::submit(const NewOrder& req, EventSink& sink) {
    stats_.orders_received++;
    SubmitResult res;

    if (index_.find(req.client_id) != index_.end()) {
        res.reason = RejectReason::DuplicateClientOrderId;
        emit_reject(sink, req.client_id, res.reason);
        return res;
    }

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

    if (req.type == OrdType::FOK) {
        if (available_to(req.side, req.price, req.participant) < req.quantity) {
            res.reason = RejectReason::FokUnfillable;
            emit_reject(sink, req.client_id, res.reason);
            return res;
        }
    }

    // The taker is a stack local until it is known to rest. Only then does it
    // consume a pool slot, so orders that fill immediately never touch the pool.
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
        res.status = OrderStatus::Filled;
        emit_ack(sink, req.client_id, OrderStatus::Filled);
        return res;
    }

    switch (req.type) {
        case OrdType::Limit:
        case OrdType::PostOnly:
            if (rest_order(req, taker, sink)) {
                res.resting = true;
                res.status  = (taker.filled > 0) ? OrderStatus::PartiallyFilled
                                                 : OrderStatus::New;
            } else {
                res.accepted = false;
                res.reason   = RejectReason::BookFull;
                res.status   = OrderStatus::Rejected;
            }
            break;

        case OrdType::Market:
        case OrdType::IOC:
            res.status = OrderStatus::Cancelled;
            if (filled == 0) {
                stats_.rejects[static_cast<std::size_t>(RejectReason::NoLiquidity)]++;
            }
            emit_cancel(sink, req.client_id);
            break;

        case OrdType::FOK:
            res.status = OrderStatus::Cancelled;
            emit_cancel(sink, req.client_id);
            break;
    }

    return res;
}

CancelResult V1Book::cancel(ClientOrderId id, ParticipantId participant, EventSink& sink) {
    stats_.cancels_received++;
    CancelResult res;

    auto it = index_.find(id);
    if (it == index_.end()) {
        res.reason = RejectReason::UnknownOrder;
        emit_reject(sink, id, res.reason);
        return res;
    }

    Order* o = it->second;
    if (o->participant != participant) {
        res.reason = RejectReason::UnknownOrder;
        emit_reject(sink, id, res.reason);
        return res;
    }
    if (is_terminal(o->status)) {
        res.reason = RejectReason::OrderAlreadyTerminal;
        emit_reject(sink, id, res.reason);
        return res;
    }

    res.cancelled_qty = o->remaining();
    o->status = OrderStatus::Cancelled;
    remove_resting(o);

    res.accepted = true;
    stats_.cancels_accepted++;
    emit_cancel(sink, id);
    return res;
}

ReplaceResult V1Book::replace(const ReplaceRequest& req, EventSink& sink) {
    stats_.replaces_received++;
    ReplaceResult res;

    auto it = index_.find(req.original_client_id);
    if (it == index_.end()) {
        res.reason = RejectReason::UnknownOrder;
        emit_reject(sink, req.original_client_id, res.reason);
        return res;
    }

    Order* o = it->second;
    if (o->participant != req.participant) {
        res.reason = RejectReason::UnknownOrder;
        emit_reject(sink, req.original_client_id, res.reason);
        return res;
    }
    if (is_terminal(o->status)) {
        res.reason = RejectReason::OrderAlreadyTerminal;
        emit_reject(sink, req.original_client_id, res.reason);
        return res;
    }

    const Side    side      = o->side;
    const OrdType type      = o->type;
    const Price   old_price = o->price;
    const Qty     already   = o->filled;

    const bool price_unchanged = (req.new_price == kNoPrice || req.new_price == old_price);
    if (price_unchanged && req.new_quantity > already && req.new_quantity <= o->quantity) {
        // In-place shrink keeps time priority. The level aggregate must be
        // adjusted by the delta, since the order stays linked.
        const Qty delta = o->quantity - req.new_quantity;
        o->quantity = req.new_quantity;
        LevelMap& m = side_map(side);
        auto lit = m.find(old_price);
        if (lit != m.end()) lit->second.reduce(delta);

        res.accepted      = true;
        res.priority_kept = true;
        res.remaining     = o->remaining();
        res.new_order_id  = o->id;
        stats_.replaces_accepted++;
        emit_ack(sink, req.original_client_id, o->status);
        return res;
    }

    o->status = OrderStatus::Cancelled;
    remove_resting(o);
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

bool V1Book::best_bid(Price& out) const {
    if (bids_.empty()) return false;
    out = std::prev(bids_.end())->first;
    return true;
}

bool V1Book::best_ask(Price& out) const {
    if (asks_.empty()) return false;
    out = asks_.begin()->first;
    return true;
}

BookSnapshot V1Book::snapshot(std::size_t depth) const {
    BookSnapshot snap;
    snap.seq = seq_;

    std::size_t n = 0;
    for (auto it = bids_.rbegin(); it != bids_.rend() && n < depth; ++it, ++n) {
        snap.bids.push_back(LevelView{it->first, it->second.total_qty, it->second.count});
    }
    n = 0;
    for (auto it = asks_.begin(); it != asks_.end() && n < depth; ++it, ++n) {
        snap.asks.push_back(LevelView{it->first, it->second.total_qty, it->second.count});
    }
    return snap;
}

bool V1Book::check_invariants(std::string& detail) const {
    if (!bids_.empty() && !asks_.empty()) {
        const Price bb = std::prev(bids_.end())->first;
        const Price ba = asks_.begin()->first;
        if (bb >= ba) {
            detail += "crossed book: best_bid=" + std::to_string(bb) +
                      " >= best_ask=" + std::to_string(ba) + "\n";
            return false;
        }
    }

    std::size_t counted = 0;
    auto check_side = [&](const LevelMap& m, const char* name) -> bool {
        for (const auto& [px, lvl] : m) {
            if (lvl.empty()) {
                detail += std::string("empty ") + name + " level at " + std::to_string(px) + "\n";
                return false;
            }
            // The incrementally-maintained aggregate against a fresh walk. This
            // redundancy is the whole point of the check.
            const Qty walked = lvl.walk_qty();
            if (walked != lvl.total_qty) {
                detail += std::string(name) + " level " + std::to_string(px) +
                          " aggregate " + std::to_string(lvl.total_qty) +
                          " != walked " + std::to_string(walked) + "\n";
                return false;
            }
            const std::uint32_t wc = lvl.walk_count();
            if (wc != lvl.count) {
                detail += std::string(name) + " level " + std::to_string(px) +
                          " count " + std::to_string(lvl.count) +
                          " != walked " + std::to_string(wc) + "\n";
                return false;
            }
            counted += wc;
        }
        return true;
    };

    if (!check_side(bids_, "bid")) return false;
    if (!check_side(asks_, "ask")) return false;

    if (counted != index_.size()) {
        detail += "resting count " + std::to_string(counted) +
                  " != index size " + std::to_string(index_.size()) + "\n";
        return false;
    }
    if (index_.size() != pool_.in_use()) {
        detail += "index size " + std::to_string(index_.size()) +
                  " != pool in_use " + std::to_string(pool_.in_use()) + "\n";
        return false;
    }
    if (stats_.buy_filled_qty != stats_.sell_filled_qty) {
        detail += "conservation broken: buy_filled=" +
                  std::to_string(stats_.buy_filled_qty) + " sell_filled=" +
                  std::to_string(stats_.sell_filled_qty) + "\n";
        return false;
    }
    if (stats_.buy_notional != stats_.sell_notional) {
        detail += "notional reconciliation broken\n";
        return false;
    }
    return true;
}

std::string V1Book::debug_dump() const {
    std::ostringstream os;
    os << "=== V1 book dump: " << cfg_.symbol << " ===\n";
    os << "seq=" << seq_ << " resting=" << index_.size()
       << " pool_in_use=" << pool_.in_use() << "/" << pool_.capacity()
       << " fills=" << stats_.fills << "\n";
    os << "-- asks (ascending) --\n";
    for (const auto& [px, lvl] : asks_) {
        os << "  " << px << "  qty=" << lvl.total_qty << "  orders=" << lvl.count << "\n";
    }
    os << "-- bids (descending) --\n";
    for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
        os << "  " << it->first << "  qty=" << it->second.total_qty
           << "  orders=" << it->second.count << "\n";
    }
    return os.str();
}

}  // namespace

std::unique_ptr<IBook> make(const BookConfig& cfg) {
    return std::make_unique<V1Book>(cfg);
}

}  // namespace matchbook::v1
