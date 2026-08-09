// V2 — flat price-level array with bitmap-tracked top of book.
//
// What changed from V1:
//   * std::map<Price, IntrusiveList> becomes std::vector<IntrusiveList> indexed
//     by (price - base) / tick_size. Locating a price level is one multiply-free
//     subtract, a divide by a runtime constant, and one indexed load, instead of
//     a red-black tree descent of ~log2(levels) nodes with a likely cache miss
//     at each.
//   * A two-level occupancy bitmap answers "best bid" and "best ask" without
//     scanning the array.
//
// WHEN THIS IS THE WRONG DESIGN
// -----------------------------
// A flat array trades memory for indexing speed, and the trade is only good
// when the price range is narrow and reasonably dense. Concretely:
//
//   * Memory is paid up front for every representable level whether it is used
//     or not. With flat_levels = 65,536 and two sides, that is roughly 4 MB of
//     level structures for a book that may hold ten live levels. On a machine
//     with ~16 MB of L3 shared across cores, a handful of symbols configured
//     this way will evict each other and every other tenant.
//
//   * Any price outside [base, base + levels*tick] cannot be represented at
//     all. V2 rejects those with PriceOutsideArray rather than silently
//     mis-indexing. A venue with a genuinely wide range -- an illiquid
//     instrument quoted from 0.0001 to 10,000, or an asset that gapped through
//     the configured band -- will see legitimate orders rejected.
//
//   * Sparse books make the bitmap scan longer in the tail. The summary level
//     bounds it well, but a book with two orders 40,000 ticks apart still walks
//     more summary words than a tree descent would have taken nodes.
//
// The honest summary: this design suits a liquid instrument with a tick size
// chosen so that active trading spans hundreds, not tens of thousands, of
// levels. That is a real and common case -- it is most listed futures and
// large-cap equities -- but it is a precondition, not a given. docs/
// expected-performance.md names this as the optimisation most likely to
// underdeliver or regress.

#include "v2_book.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/bitmap.hpp"
#include "../common/order_pool.hpp"

namespace matchbook::v2 {
namespace {

using detail::IntrusiveList;
using detail::LevelBitmap;
using detail::OrderPool;

class V2Book final : public IBook {
public:
    explicit V2Book(const BookConfig& cfg)
        : cfg_(cfg),
          pool_(cfg.max_orders),
          base_price_(cfg.flat_base_price()),
          n_levels_(cfg.flat_levels),
          bid_levels_(cfg.flat_levels),
          ask_levels_(cfg.flat_levels),
          bid_map_(cfg.flat_levels),
          ask_map_(cfg.flat_levels) {
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
    [[nodiscard]] EngineVersion version() const override { return EngineVersion::V2_Flat; }
    [[nodiscard]] bool check_invariants(std::string& detail) const override;
    [[nodiscard]] std::string debug_dump() const override;

private:
    // Maps a price onto an array index. Returns false when the price falls
    // outside the representable window -- the case that must be handled
    // explicitly rather than wrapping or clamping.
    [[nodiscard]] bool price_to_index(Price p, std::size_t& idx) const noexcept {
        if (p < base_price_) return false;
        const Price offset = p - base_price_;
        if ((offset % cfg_.tick_size) != 0) return false;
        const auto i = static_cast<std::size_t>(offset / cfg_.tick_size);
        if (i >= n_levels_) return false;
        idx = i;
        return true;
    }

    [[nodiscard]] Price index_to_price(std::size_t idx) const noexcept {
        return base_price_ + static_cast<Price>(idx) * cfg_.tick_size;
    }

    std::vector<IntrusiveList>& levels(Side s) noexcept {
        return s == Side::Buy ? bid_levels_ : ask_levels_;
    }
    const std::vector<IntrusiveList>& levels(Side s) const noexcept {
        return s == Side::Buy ? bid_levels_ : ask_levels_;
    }
    LevelBitmap& map_for(Side s) noexcept { return s == Side::Buy ? bid_map_ : ask_map_; }
    const LevelBitmap& map_for(Side s) const noexcept { return s == Side::Buy ? bid_map_ : ask_map_; }

    [[nodiscard]] Qty available_to(Side taker_side, Price limit, ParticipantId participant) const;

    void emit_ack(EventSink& sink, ClientOrderId cid, OrderStatus st);
    void emit_reject(EventSink& sink, ClientOrderId cid, RejectReason r);
    void emit_cancel(EventSink& sink, ClientOrderId cid);
    void emit_fill(EventSink& sink, ClientOrderId taker, ClientOrderId maker,
                   Price px, Qty qty, Side taker_side);

    Qty  match(const NewOrder& req, Order& taker, EventSink& sink);
    bool rest_order(const NewOrder& req, const Order& taker, std::size_t idx, EventSink& sink);
    void remove_resting(Order* o);

    BookConfig cfg_;
    OrderPool  pool_;
    Price      base_price_;
    std::size_t n_levels_;

    std::vector<IntrusiveList> bid_levels_;
    std::vector<IntrusiveList> ask_levels_;
    LevelBitmap bid_map_;
    LevelBitmap ask_map_;

    std::unordered_map<ClientOrderId, Order*> index_;
    Stats   stats_;
    SeqNum  seq_     = 0;
    OrderId next_id_ = 1;
};

// ---------------------------------------------------------------------------
// events
// ---------------------------------------------------------------------------

void V2Book::emit_ack(EventSink& sink, ClientOrderId cid, OrderStatus st) {
    Event e; e.kind = EventKind::Ack; e.seq = ++seq_; e.client_id = cid; e.status = st;
    sink.push(e);
}

void V2Book::emit_reject(EventSink& sink, ClientOrderId cid, RejectReason r) {
    Event e; e.kind = EventKind::Reject; e.seq = ++seq_; e.client_id = cid;
    e.reason = r; e.status = OrderStatus::Rejected;
    sink.push(e);
    stats_.count_reject(r);
}

void V2Book::emit_cancel(EventSink& sink, ClientOrderId cid) {
    Event e; e.kind = EventKind::Cancel; e.seq = ++seq_; e.client_id = cid;
    e.status = OrderStatus::Cancelled;
    sink.push(e);
}

void V2Book::emit_fill(EventSink& sink, ClientOrderId taker, ClientOrderId maker,
                       Price px, Qty qty, Side taker_side) {
    Event e;
    e.kind = EventKind::Fill; e.seq = ++seq_;
    e.client_id = taker; e.maker_client_id = maker;
    e.price = px; e.quantity = qty; e.side = taker_side;
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

Qty V2Book::available_to(Side taker_side, Price limit, ParticipantId participant) const {
    const Side opp_side = opposite(taker_side);
    const LevelBitmap& bm = map_for(opp_side);
    const std::vector<IntrusiveList>& lv = levels(opp_side);

    Qty total = 0;
    std::size_t idx = 0;

    if (taker_side == Side::Buy) {
        // Buyer walks asks upward from the best (lowest) price.
        if (!bm.find_first(idx)) return 0;
        std::size_t limit_idx = n_levels_ - 1;
        if (limit != kNoPrice) {
            std::size_t li = 0;
            if (!price_to_index(limit, li)) {
                // A limit below the array base can cross nothing.
                if (limit < base_price_) return 0;
                li = n_levels_ - 1;
            }
            limit_idx = li;
        }
        for (;;) {
            if (idx > limit_idx) break;
            for (const Order* o = lv[idx].head; o != nullptr; o = o->next) {
                if (o->participant == participant) continue;
                total += o->remaining();
            }
            std::size_t nxt = 0;
            if (!bm.find_next(idx, nxt)) break;
            idx = nxt;
        }
    } else {
        // Seller walks bids downward from the best (highest) price.
        if (!bm.find_last(idx)) return 0;
        std::size_t limit_idx = 0;
        if (limit != kNoPrice) {
            std::size_t li = 0;
            if (!price_to_index(limit, li)) {
                if (limit > index_to_price(n_levels_ - 1)) return 0;
                li = 0;
            }
            limit_idx = li;
        }
        for (;;) {
            if (idx < limit_idx) break;
            for (const Order* o = lv[idx].head; o != nullptr; o = o->next) {
                if (o->participant == participant) continue;
                total += o->remaining();
            }
            std::size_t prv = 0;
            if (!bm.find_prev(idx, prv)) break;
            idx = prv;
        }
    }
    return total;
}

// ---------------------------------------------------------------------------
// matching
// ---------------------------------------------------------------------------

Qty V2Book::match(const NewOrder& req, Order& taker, EventSink& sink) {
    const Side opp_side = opposite(req.side);
    LevelBitmap& bm = map_for(opp_side);
    std::vector<IntrusiveList>& lv = levels(opp_side);

    Qty filled = 0;
    const bool buying = (req.side == Side::Buy);

    // Resolve the taker's price limit to an index once, outside the loop.
    std::size_t limit_idx = buying ? n_levels_ - 1 : 0;
    if (req.price != kNoPrice) {
        std::size_t li = 0;
        if (price_to_index(req.price, li)) {
            limit_idx = li;
        } else if (buying) {
            if (req.price < base_price_) return 0;   // crosses nothing
            limit_idx = n_levels_ - 1;               // above the array: crosses everything
        } else {
            if (req.price > index_to_price(n_levels_ - 1)) return 0;
            limit_idx = 0;
        }
    }

    std::size_t idx = 0;
    bool have = buying ? bm.find_first(idx) : bm.find_last(idx);

    while (have && taker.remaining() > 0) {
        if (buying ? (idx > limit_idx) : (idx < limit_idx)) break;

        IntrusiveList& queue = lv[idx];

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

        if (queue.empty()) {
            // Clearing the bitmap bit is what makes this level invisible to the
            // next best-bid/ask query. The level struct itself stays allocated;
            // that is the whole design.
            bm.clear(idx);
            queue.clear();
        }

        std::size_t nxt = 0;
        have = buying ? bm.find_next(idx, nxt) : bm.find_prev(idx, nxt);
        idx = nxt;
    }

    return filled;
}

bool V2Book::rest_order(const NewOrder& req, const Order& taker, std::size_t idx,
                        EventSink& sink) {
    Order* o = pool_.acquire();
    if (o == nullptr) {
        emit_reject(sink, req.client_id, RejectReason::BookFull);
        return false;
    }

    const std::int32_t slot = o->pool_slot;
    *o = taker;
    o->pool_slot   = slot;
    o->level_index = static_cast<std::int32_t>(idx);
    o->status = (o->filled > 0) ? OrderStatus::PartiallyFilled : OrderStatus::New;

    levels(req.side)[idx].push_back(o);
    map_for(req.side).set(idx);
    index_.emplace(req.client_id, o);
    stats_.orders_resting = index_.size();

    emit_ack(sink, req.client_id, o->status);
    return true;
}

void V2Book::remove_resting(Order* o) {
    const auto idx = static_cast<std::size_t>(o->level_index);
    if (o->level_index < 0 || idx >= n_levels_) return;

    IntrusiveList& queue = levels(o->side)[idx];
    queue.unlink(o);
    if (queue.empty()) {
        map_for(o->side).clear(idx);
        queue.clear();
    }
    index_.erase(o->client_id);
    pool_.release(o);
    stats_.orders_resting = index_.size();
}

// ---------------------------------------------------------------------------
// operations
// ---------------------------------------------------------------------------

SubmitResult V2Book::submit(const NewOrder& req, EventSink& sink) {
    stats_.orders_received++;
    SubmitResult res;

    if (index_.find(req.client_id) != index_.end()) {
        res.reason = RejectReason::DuplicateClientOrderId;
        emit_reject(sink, req.client_id, res.reason);
        return res;
    }

    // A resting order needs a representable price. Market/IOC orders that never
    // rest are exempt, which is why this is checked per-type rather than up
    // front.
    std::size_t rest_idx = 0;
    const bool may_rest = (req.type == OrdType::Limit || req.type == OrdType::PostOnly);
    if (may_rest) {
        if (req.price == kNoPrice) {
            res.reason = RejectReason::LimitOrderWithoutPrice;
            emit_reject(sink, req.client_id, res.reason);
            return res;
        }
        if (!price_to_index(req.price, rest_idx)) {
            res.reason = RejectReason::PriceOutsideArray;
            emit_reject(sink, req.client_id, res.reason);
            return res;
        }
    }

    if (req.type == OrdType::PostOnly) {
        Price best = 0;
        const bool have_opp = (req.side == Side::Buy) ? best_ask(best) : best_bid(best);
        if (have_opp) {
            const bool would_cross = (req.side == Side::Buy) ? (req.price >= best)
                                                             : (req.price <= best);
            if (would_cross) {
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
            if (rest_order(req, taker, rest_idx, sink)) {
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

CancelResult V2Book::cancel(ClientOrderId id, ParticipantId participant, EventSink& sink) {
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

ReplaceResult V2Book::replace(const ReplaceRequest& req, EventSink& sink) {
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
    const auto    old_idx   = static_cast<std::size_t>(o->level_index);

    const bool price_unchanged = (req.new_price == kNoPrice || req.new_price == old_price);
    if (price_unchanged && req.new_quantity > already && req.new_quantity <= o->quantity) {
        const Qty delta = o->quantity - req.new_quantity;
        o->quantity = req.new_quantity;
        if (o->level_index >= 0 && old_idx < n_levels_) {
            levels(side)[old_idx].reduce(delta);
        }
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

bool V2Book::best_bid(Price& out) const {
    std::size_t idx = 0;
    if (!bid_map_.find_last(idx)) return false;
    out = index_to_price(idx);
    return true;
}

bool V2Book::best_ask(Price& out) const {
    std::size_t idx = 0;
    if (!ask_map_.find_first(idx)) return false;
    out = index_to_price(idx);
    return true;
}

BookSnapshot V2Book::snapshot(std::size_t depth) const {
    BookSnapshot snap;
    snap.seq = seq_;

    std::size_t idx = 0;
    if (bid_map_.find_last(idx)) {
        for (std::size_t n = 0; n < depth; ++n) {
            const IntrusiveList& lvl = bid_levels_[idx];
            snap.bids.push_back(LevelView{index_to_price(idx), lvl.total_qty, lvl.count});
            std::size_t prv = 0;
            if (!bid_map_.find_prev(idx, prv)) break;
            idx = prv;
        }
    }
    if (ask_map_.find_first(idx)) {
        for (std::size_t n = 0; n < depth; ++n) {
            const IntrusiveList& lvl = ask_levels_[idx];
            snap.asks.push_back(LevelView{index_to_price(idx), lvl.total_qty, lvl.count});
            std::size_t nxt = 0;
            if (!ask_map_.find_next(idx, nxt)) break;
            idx = nxt;
        }
    }
    return snap;
}

bool V2Book::check_invariants(std::string& detail) const {
    Price bb = 0, ba = 0;
    const bool has_bid = best_bid(bb);
    const bool has_ask = best_ask(ba);
    if (has_bid && has_ask && bb >= ba) {
        detail += "crossed book: best_bid=" + std::to_string(bb) +
                  " >= best_ask=" + std::to_string(ba) + "\n";
        return false;
    }

    std::size_t counted = 0;
    auto check_side = [&](const std::vector<IntrusiveList>& lv, const LevelBitmap& bm,
                          const char* name) -> bool {
        for (std::size_t i = 0; i < n_levels_; ++i) {
            const IntrusiveList& lvl = lv[i];
            const bool occupied = bm.test(i);
            // The bitmap and the level array must agree. They are two
            // representations of the same fact, updated at different call
            // sites, which is exactly where drift happens.
            if (occupied != !lvl.empty()) {
                detail += std::string(name) + " level " + std::to_string(i) +
                          ": bitmap says " + (occupied ? "occupied" : "empty") +
                          " but list is " + (lvl.empty() ? "empty" : "occupied") + "\n";
                return false;
            }
            if (!occupied) continue;

            const Qty walked = lvl.walk_qty();
            if (walked != lvl.total_qty) {
                detail += std::string(name) + " level " + std::to_string(i) +
                          " aggregate " + std::to_string(lvl.total_qty) +
                          " != walked " + std::to_string(walked) + "\n";
                return false;
            }
            const std::uint32_t wc = lvl.walk_count();
            if (wc != lvl.count) {
                detail += std::string(name) + " level " + std::to_string(i) +
                          " count mismatch\n";
                return false;
            }
            counted += wc;
        }
        return true;
    };

    if (!check_side(bid_levels_, bid_map_, "bid")) return false;
    if (!check_side(ask_levels_, ask_map_, "ask")) return false;

    if (counted != index_.size()) {
        detail += "resting count " + std::to_string(counted) +
                  " != index size " + std::to_string(index_.size()) + "\n";
        return false;
    }
    if (index_.size() != pool_.in_use()) {
        detail += "index size != pool in_use\n";
        return false;
    }
    if (stats_.buy_filled_qty != stats_.sell_filled_qty) {
        detail += "conservation broken\n";
        return false;
    }
    if (stats_.buy_notional != stats_.sell_notional) {
        detail += "notional reconciliation broken\n";
        return false;
    }
    return true;
}

std::string V2Book::debug_dump() const {
    std::ostringstream os;
    os << "=== V2 book dump: " << cfg_.symbol << " ===\n";
    os << "seq=" << seq_ << " resting=" << index_.size()
       << " pool_in_use=" << pool_.in_use() << "/" << pool_.capacity()
       << " base_price=" << base_price_ << " levels=" << n_levels_
       << " bid_bits=" << bid_map_.count() << " ask_bits=" << ask_map_.count()
       << " fills=" << stats_.fills << "\n";

    const BookSnapshot snap = snapshot(20);
    os << "-- asks (ascending) --\n";
    for (const LevelView& l : snap.asks) {
        os << "  " << l.price << "  qty=" << l.quantity << "  orders=" << l.order_count << "\n";
    }
    os << "-- bids (descending) --\n";
    for (const LevelView& l : snap.bids) {
        os << "  " << l.price << "  qty=" << l.quantity << "  orders=" << l.order_count << "\n";
    }
    return os.str();
}

}  // namespace

std::unique_ptr<IBook> make(const BookConfig& cfg) {
    return std::make_unique<V2Book>(cfg);
}

}  // namespace matchbook::v2
