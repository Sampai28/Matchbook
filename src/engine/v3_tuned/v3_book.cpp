// V3 — hot-path tuning.
//
// Four changes from V2, each targeting a specific measured-in-principle cost.
// Whether any of them pays is exactly what `make bench` is for; see
// docs/expected-performance.md for what is expected and why.
//
// 1. A COMPACT ORDER RECORD (HotOrder, below).
//    The shared matchbook::Order is ~88 bytes and straddles two cache lines.
//    Matching reads price, remaining quantity, participant and the next pointer
//    on every iteration; in V2 those are spread across both lines, so each
//    maker costs two line fetches. HotOrder packs exactly those fields into the
//    first 32 bytes, so the inner loop touches one line per maker.
//
// 2. CACHED TOP-OF-BOOK INDICES.
//    V2 calls bitmap.find_first()/find_last() on every submit. Those are cheap
//    but not free: a summary probe, a word probe, a TZCNT. V3 caches the best
//    index per side and only rescans when the cached level empties, which is
//    the uncommon case.
//
// 3. POWER-OF-TWO TICK FAST PATH.
//    price_to_index divides by tick_size. Integer division by a runtime value
//    is 20-40 cycles on Zen and cannot be strength-reduced by the compiler
//    because tick_size is not a compile-time constant. When tick_size is a
//    power of two -- the common configuration -- V3 substitutes a shift.
//
// 4. BRANCH HINTS ON VALIDATED FAST PATHS.
//    MB_LIKELY/MB_UNLIKELY are applied only where the direction is genuinely
//    lopsided and already validated: rejections are rare, pool exhaustion is
//    rare, self-trade collisions are rare. Hinting a balanced branch makes
//    things worse, so they are used sparingly and each one is justified at its
//    site.

#include "v3_book.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/bitmap.hpp"

// __builtin_expect tells the compiler which way a branch usually goes, so it can
// lay out the likely path as the fall-through and keep the unlikely path off the
// hot instruction cache lines. C++20's [[likely]]/[[unlikely]] attributes do the
// same thing portably; the macro form is used here because it reads better
// inside an if-condition.
#if defined(__GNUC__) || defined(__clang__)
#  define MB_LIKELY(x)   (__builtin_expect(!!(x), 1))
#  define MB_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
#  define MB_LIKELY(x)   (x)
#  define MB_UNLIKELY(x) (x)
#endif

namespace matchbook::v3 {
namespace {

using detail::LevelBitmap;

// ---------------------------------------------------------------------------
// compact resting-order record
// ---------------------------------------------------------------------------

// Field order is deliberate, not alphabetical or logical. The first 32 bytes
// hold everything the matching inner loop reads:
//
//   next (8) | remaining (8) | price (8) | participant (4) | flags (4)
//
// That is one half of a 64-byte line, so two consecutive makers in the same
// level often share a line when the pool hands out adjacent slots.
//
// The colder fields -- client id, original quantity, sequence -- follow. They
// are read once per fill (to emit the event), not once per loop iteration.
struct HotOrder {
    HotOrder*     next        = nullptr;   //  0
    Qty           remaining   = 0;         //  8
    Price         price       = 0;         // 16
    ParticipantId participant = 0;         // 24
    std::uint8_t  side        = 0;         // 28
    std::uint8_t  type        = 0;         // 29
    std::uint8_t  status      = 0;         // 30
    std::uint8_t  pad0        = 0;         // 31

    // --- cold half ---
    HotOrder*     prev        = nullptr;   // 32
    ClientOrderId client_id   = 0;         // 40
    Qty           quantity    = 0;         // 48
    OrderId       id          = 0;         // 56
    std::int32_t  level_index = -1;        // 64
    std::int32_t  pool_slot   = -1;        // 68

    [[nodiscard]] Qty filled() const noexcept { return quantity - remaining; }
};

// Intrusive level over HotOrder, with the aggregate kept adjacent to the head
// pointer so a depth query touches one line.
struct alignas(64) HotLevel {
    HotOrder*     head      = nullptr;
    HotOrder*     tail      = nullptr;
    Qty           total_qty = 0;
    std::uint32_t count     = 0;
    std::uint32_t pad       = 0;

    [[nodiscard]] bool empty() const noexcept { return head == nullptr; }

    void push_back(HotOrder* o) noexcept {
        o->prev = tail;
        o->next = nullptr;
        if (tail != nullptr) tail->next = o; else head = o;
        tail = o;
        ++count;
        total_qty += o->remaining;
    }

    void unlink(HotOrder* o) noexcept {
        if (o->prev != nullptr) o->prev->next = o->next; else head = o->next;
        if (o->next != nullptr) o->next->prev = o->prev; else tail = o->prev;
        o->prev = o->next = nullptr;
        --count;
        total_qty -= o->remaining;
    }

    void reduce(Qty by) noexcept { total_qty -= by; }
    void reset() noexcept { head = tail = nullptr; count = 0; total_qty = 0; }

    [[nodiscard]] Qty walk_qty() const noexcept {
        Qty s = 0;
        for (const HotOrder* o = head; o != nullptr; o = o->next) s += o->remaining;
        return s;
    }
    [[nodiscard]] std::uint32_t walk_count() const noexcept {
        std::uint32_t n = 0;
        for (const HotOrder* o = head; o != nullptr; o = o->next) ++n;
        return n;
    }
};

class HotPool {
public:
    explicit HotPool(std::size_t capacity) : storage_(capacity) {
        for (std::size_t i = capacity; i-- > 0;) {
            storage_[i].pool_slot = static_cast<std::int32_t>(i);
            storage_[i].next = free_head_;
            free_head_ = &storage_[i];
        }
    }

    [[nodiscard]] HotOrder* acquire() noexcept {
        if (MB_UNLIKELY(free_head_ == nullptr)) return nullptr;
        HotOrder* o = free_head_;
        free_head_ = o->next;
        const std::int32_t slot = o->pool_slot;
        *o = HotOrder{};
        o->pool_slot = slot;
        ++in_use_;
        return o;
    }

    void release(HotOrder* o) noexcept {
        o->prev = nullptr;
        o->level_index = -1;
        o->next = free_head_;
        free_head_ = o;
        --in_use_;
    }

    [[nodiscard]] std::size_t in_use() const noexcept { return in_use_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return storage_.size(); }

private:
    std::vector<HotOrder> storage_;
    HotOrder*   free_head_ = nullptr;
    std::size_t in_use_    = 0;
};

// ---------------------------------------------------------------------------
// book
// ---------------------------------------------------------------------------

class V3Book final : public IBook {
public:
    explicit V3Book(const BookConfig& cfg)
        : cfg_(cfg),
          base_price_(cfg.flat_base_price()),
          tick_(cfg.tick_size),
          n_levels_(cfg.flat_levels),
          tick_shift_(compute_tick_shift(cfg.tick_size)),
          pool_(cfg.max_orders),
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
    [[nodiscard]] EngineVersion version() const override { return EngineVersion::V3_Tuned; }
    [[nodiscard]] bool check_invariants(std::string& detail) const override;
    [[nodiscard]] std::string debug_dump() const override;

private:
    // -1 means "not a power of two"; the divide fallback is used instead.
    static int compute_tick_shift(Price tick) noexcept {
        if (tick <= 0) return -1;
        const auto u = static_cast<std::uint64_t>(tick);
        if ((u & (u - 1)) != 0) return -1;             // not a power of two
        return std::countr_zero(u);
    }

    [[nodiscard]] bool price_to_index(Price p, std::size_t& idx) const noexcept {
        if (MB_UNLIKELY(p < base_price_)) return false;
        const Price offset = p - base_price_;
        std::size_t i;
        if (MB_LIKELY(tick_shift_ >= 0)) {
            // Power-of-two tick: the divide becomes a shift, and the remainder
            // test becomes a mask.
            if (MB_UNLIKELY((offset & (tick_ - 1)) != 0)) return false;
            i = static_cast<std::size_t>(offset >> tick_shift_);
        } else {
            if (MB_UNLIKELY((offset % tick_) != 0)) return false;
            i = static_cast<std::size_t>(offset / tick_);
        }
        if (MB_UNLIKELY(i >= n_levels_)) return false;
        idx = i;
        return true;
    }

    [[nodiscard]] Price index_to_price(std::size_t idx) const noexcept {
        return base_price_ + static_cast<Price>(idx) * tick_;
    }

    std::vector<HotLevel>& levels(Side s) noexcept {
        return s == Side::Buy ? bid_levels_ : ask_levels_;
    }
    const std::vector<HotLevel>& levels(Side s) const noexcept {
        return s == Side::Buy ? bid_levels_ : ask_levels_;
    }
    LevelBitmap& map_for(Side s) noexcept { return s == Side::Buy ? bid_map_ : ask_map_; }
    const LevelBitmap& map_for(Side s) const noexcept {
        return s == Side::Buy ? bid_map_ : ask_map_;
    }

    // Cached top-of-book. best_bid_idx_ == kNoIdx means "no bid side".
    static constexpr std::size_t kNoIdx = static_cast<std::size_t>(-1);

    void refresh_best(Side s) noexcept {
        std::size_t idx = 0;
        if (s == Side::Buy) {
            best_bid_idx_ = bid_map_.find_last(idx) ? idx : kNoIdx;
        } else {
            best_ask_idx_ = ask_map_.find_first(idx) ? idx : kNoIdx;
        }
    }

    void on_level_occupied(Side s, std::size_t idx) noexcept {
        map_for(s).set(idx);
        if (s == Side::Buy) {
            if (best_bid_idx_ == kNoIdx || idx > best_bid_idx_) best_bid_idx_ = idx;
        } else {
            if (best_ask_idx_ == kNoIdx || idx < best_ask_idx_) best_ask_idx_ = idx;
        }
    }

    void on_level_emptied(Side s, std::size_t idx) noexcept {
        map_for(s).clear(idx);
        // Only rescan when the level that emptied was the top of book. Every
        // other emptying leaves the cached best untouched, which is the point.
        if (s == Side::Buy) {
            if (idx == best_bid_idx_) refresh_best(Side::Buy);
        } else {
            if (idx == best_ask_idx_) refresh_best(Side::Sell);
        }
    }

    [[nodiscard]] Qty available_to(Side taker_side, Price limit, ParticipantId participant) const;

    void emit_ack(EventSink& sink, ClientOrderId cid, OrderStatus st);
    void emit_reject(EventSink& sink, ClientOrderId cid, RejectReason r);
    void emit_cancel(EventSink& sink, ClientOrderId cid);
    void emit_fill(EventSink& sink, ClientOrderId taker, ClientOrderId maker,
                   Price px, Qty qty, Side taker_side);

    Qty  match(const NewOrder& req, Qty& taker_remaining, EventSink& sink);
    bool rest_order(const NewOrder& req, Qty remaining, std::size_t idx, EventSink& sink);
    void remove_resting(HotOrder* o);

    // Field order below is deliberate. base_price_, tick_, n_levels_ and
    // tick_shift_ are read by price_to_index on every submit, so they are
    // declared adjacently and land on the same cache line as the object header.
    // cfg_ is a std::string-bearing struct read only on the cold path; it sits
    // first because moving it after the hot scalars would push them past the
    // 64-byte boundary.
    BookConfig  cfg_;
    Price       base_price_;
    Price       tick_;
    std::size_t n_levels_;
    int         tick_shift_;

    HotPool     pool_;
    std::vector<HotLevel> bid_levels_;
    std::vector<HotLevel> ask_levels_;
    LevelBitmap bid_map_;
    LevelBitmap ask_map_;

    std::size_t best_bid_idx_ = kNoIdx;
    std::size_t best_ask_idx_ = kNoIdx;

    std::unordered_map<ClientOrderId, HotOrder*> index_;
    Stats   stats_;
    SeqNum  seq_     = 0;
    OrderId next_id_ = 1;
};

// ---------------------------------------------------------------------------
// events
// ---------------------------------------------------------------------------

void V3Book::emit_ack(EventSink& sink, ClientOrderId cid, OrderStatus st) {
    Event e; e.kind = EventKind::Ack; e.seq = ++seq_; e.client_id = cid; e.status = st;
    sink.push(e);
}

void V3Book::emit_reject(EventSink& sink, ClientOrderId cid, RejectReason r) {
    Event e; e.kind = EventKind::Reject; e.seq = ++seq_; e.client_id = cid;
    e.reason = r; e.status = OrderStatus::Rejected;
    sink.push(e);
    stats_.count_reject(r);
}

void V3Book::emit_cancel(EventSink& sink, ClientOrderId cid) {
    Event e; e.kind = EventKind::Cancel; e.seq = ++seq_; e.client_id = cid;
    e.status = OrderStatus::Cancelled;
    sink.push(e);
}

void V3Book::emit_fill(EventSink& sink, ClientOrderId taker, ClientOrderId maker,
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

Qty V3Book::available_to(Side taker_side, Price limit, ParticipantId participant) const {
    const Side opp_side = opposite(taker_side);
    const LevelBitmap& bm = map_for(opp_side);
    const std::vector<HotLevel>& lv = levels(opp_side);

    Qty total = 0;
    std::size_t idx = 0;
    const bool buying = (taker_side == Side::Buy);

    if (buying) {
        if (best_ask_idx_ == kNoIdx) return 0;
        idx = best_ask_idx_;
        std::size_t limit_idx = n_levels_ - 1;
        if (limit != kNoPrice) {
            std::size_t li = 0;
            if (price_to_index(limit, li)) limit_idx = li;
            else if (limit < base_price_) return 0;
        }
        for (;;) {
            if (idx > limit_idx) break;
            for (const HotOrder* o = lv[idx].head; o != nullptr; o = o->next) {
                if (MB_UNLIKELY(o->participant == participant)) continue;
                total += o->remaining;
            }
            std::size_t nxt = 0;
            if (!bm.find_next(idx, nxt)) break;
            idx = nxt;
        }
    } else {
        if (best_bid_idx_ == kNoIdx) return 0;
        idx = best_bid_idx_;
        std::size_t limit_idx = 0;
        if (limit != kNoPrice) {
            std::size_t li = 0;
            if (price_to_index(limit, li)) limit_idx = li;
            else if (limit > index_to_price(n_levels_ - 1)) return 0;
        }
        for (;;) {
            if (idx < limit_idx) break;
            for (const HotOrder* o = lv[idx].head; o != nullptr; o = o->next) {
                if (MB_UNLIKELY(o->participant == participant)) continue;
                total += o->remaining;
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

Qty V3Book::match(const NewOrder& req, Qty& taker_remaining, EventSink& sink) {
    const Side opp_side = opposite(req.side);
    LevelBitmap& bm = map_for(opp_side);
    std::vector<HotLevel>& lv = levels(opp_side);

    const bool buying = (req.side == Side::Buy);
    std::size_t idx = buying ? best_ask_idx_ : best_bid_idx_;
    if (idx == kNoIdx) return 0;

    std::size_t limit_idx = buying ? n_levels_ - 1 : 0;
    if (req.price != kNoPrice) {
        std::size_t li = 0;
        if (MB_LIKELY(price_to_index(req.price, li))) {
            limit_idx = li;
        } else if (buying) {
            if (req.price < base_price_) return 0;
            limit_idx = n_levels_ - 1;
        } else {
            if (req.price > index_to_price(n_levels_ - 1)) return 0;
            limit_idx = 0;
        }
    }

    Qty filled = 0;

    while (taker_remaining > 0) {
        if (buying ? (idx > limit_idx) : (idx < limit_idx)) break;

        HotLevel& queue = lv[idx];

        while (taker_remaining > 0 && queue.head != nullptr) {
            HotOrder* maker = queue.head;

            // Self-trade collisions are rare in realistic flow: most orders on
            // a level belong to other participants.
            if (MB_UNLIKELY(maker->participant == req.participant)) {
                const ClientOrderId maker_cid = maker->client_id;
                queue.unlink(maker);
                index_.erase(maker_cid);
                pool_.release(maker);
                emit_cancel(sink, maker_cid);
                stats_.rejects[static_cast<std::size_t>(RejectReason::SelfTradePrevented)]++;
                continue;
            }

            const Qty   trade_qty = std::min(taker_remaining, maker->remaining);
            const Price trade_px  = maker->price;

            maker->remaining -= trade_qty;
            taker_remaining  -= trade_qty;
            filled           += trade_qty;
            queue.reduce(trade_qty);

            emit_fill(sink, req.client_id, maker->client_id, trade_px, trade_qty, req.side);

            if (maker->remaining == 0) {
                const ClientOrderId maker_cid = maker->client_id;
                queue.unlink(maker);
                index_.erase(maker_cid);
                pool_.release(maker);
            } else {
                maker->status = static_cast<std::uint8_t>(OrderStatus::PartiallyFilled);
            }
        }

        if (queue.empty()) {
            queue.reset();
            on_level_emptied(opp_side, idx);
        }

        std::size_t nxt = 0;
        const bool have = buying ? bm.find_next(idx, nxt) : bm.find_prev(idx, nxt);
        if (!have) break;
        idx = nxt;
    }

    return filled;
}

bool V3Book::rest_order(const NewOrder& req, Qty remaining, std::size_t idx, EventSink& sink) {
    HotOrder* o = pool_.acquire();
    if (MB_UNLIKELY(o == nullptr)) {
        emit_reject(sink, req.client_id, RejectReason::BookFull);
        return false;
    }

    o->client_id   = req.client_id;
    o->id          = next_id_++;
    o->participant = req.participant;
    o->price       = req.price;
    o->quantity    = req.quantity;
    o->remaining   = remaining;
    o->side        = static_cast<std::uint8_t>(req.side);
    o->type        = static_cast<std::uint8_t>(req.type);
    o->status      = static_cast<std::uint8_t>(remaining < req.quantity
                                                   ? OrderStatus::PartiallyFilled
                                                   : OrderStatus::New);
    o->level_index = static_cast<std::int32_t>(idx);

    levels(req.side)[idx].push_back(o);
    on_level_occupied(req.side, idx);
    index_.emplace(req.client_id, o);
    stats_.orders_resting = index_.size();

    emit_ack(sink, req.client_id, static_cast<OrderStatus>(o->status));
    return true;
}

void V3Book::remove_resting(HotOrder* o) {
    const auto idx = static_cast<std::size_t>(o->level_index);
    if (MB_UNLIKELY(o->level_index < 0 || idx >= n_levels_)) return;

    const Side side = static_cast<Side>(o->side);
    HotLevel& queue = levels(side)[idx];
    queue.unlink(o);
    if (queue.empty()) {
        queue.reset();
        on_level_emptied(side, idx);
    }
    index_.erase(o->client_id);
    pool_.release(o);
    stats_.orders_resting = index_.size();
}

// ---------------------------------------------------------------------------
// operations
// ---------------------------------------------------------------------------

SubmitResult V3Book::submit(const NewOrder& req, EventSink& sink) {
    stats_.orders_received++;
    SubmitResult res;

    // Duplicate client ids are a client bug, not normal flow.
    if (MB_UNLIKELY(index_.find(req.client_id) != index_.end())) {
        res.reason = RejectReason::DuplicateClientOrderId;
        emit_reject(sink, req.client_id, res.reason);
        return res;
    }

    std::size_t rest_idx = 0;
    const bool may_rest = (req.type == OrdType::Limit || req.type == OrdType::PostOnly);
    if (may_rest) {
        if (MB_UNLIKELY(req.price == kNoPrice)) {
            res.reason = RejectReason::LimitOrderWithoutPrice;
            emit_reject(sink, req.client_id, res.reason);
            return res;
        }
        if (MB_UNLIKELY(!price_to_index(req.price, rest_idx))) {
            res.reason = RejectReason::PriceOutsideArray;
            emit_reject(sink, req.client_id, res.reason);
            return res;
        }
    }

    if (MB_UNLIKELY(req.type == OrdType::PostOnly)) {
        const std::size_t opp_best = (req.side == Side::Buy) ? best_ask_idx_ : best_bid_idx_;
        if (opp_best != kNoIdx) {
            const Price best = index_to_price(opp_best);
            const bool would_cross = (req.side == Side::Buy) ? (req.price >= best)
                                                             : (req.price <= best);
            if (would_cross) {
                res.reason = RejectReason::PostOnlyWouldCross;
                emit_reject(sink, req.client_id, res.reason);
                return res;
            }
        }
    }

    if (MB_UNLIKELY(req.type == OrdType::FOK)) {
        if (available_to(req.side, req.price, req.participant) < req.quantity) {
            res.reason = RejectReason::FokUnfillable;
            emit_reject(sink, req.client_id, res.reason);
            return res;
        }
    }

    // The taker is two scalars on the stack, not a heap record. Only an order
    // that actually rests consumes a pool slot.
    Qty taker_remaining = req.quantity;
    ++seq_;

    const bool takes_liquidity = (req.type != OrdType::PostOnly);
    const Qty filled = takes_liquidity ? match(req, taker_remaining, sink) : 0;

    res.accepted   = true;
    res.order_id   = next_id_;
    res.filled_qty = filled;
    res.remaining  = taker_remaining;
    stats_.orders_accepted++;

    if (taker_remaining == 0) {
        res.status = OrderStatus::Filled;
        emit_ack(sink, req.client_id, OrderStatus::Filled);
        return res;
    }

    switch (req.type) {
        case OrdType::Limit:
        case OrdType::PostOnly:
            if (MB_LIKELY(rest_order(req, taker_remaining, rest_idx, sink))) {
                res.resting = true;
                res.status  = (filled > 0) ? OrderStatus::PartiallyFilled : OrderStatus::New;
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

CancelResult V3Book::cancel(ClientOrderId id, ParticipantId participant, EventSink& sink) {
    stats_.cancels_received++;
    CancelResult res;

    auto it = index_.find(id);
    if (MB_UNLIKELY(it == index_.end())) {
        res.reason = RejectReason::UnknownOrder;
        emit_reject(sink, id, res.reason);
        return res;
    }

    HotOrder* o = it->second;
    if (MB_UNLIKELY(o->participant != participant)) {
        res.reason = RejectReason::UnknownOrder;
        emit_reject(sink, id, res.reason);
        return res;
    }
    if (MB_UNLIKELY(is_terminal(static_cast<OrderStatus>(o->status)))) {
        res.reason = RejectReason::OrderAlreadyTerminal;
        emit_reject(sink, id, res.reason);
        return res;
    }

    res.cancelled_qty = o->remaining;
    remove_resting(o);

    res.accepted = true;
    stats_.cancels_accepted++;
    emit_cancel(sink, id);
    return res;
}

ReplaceResult V3Book::replace(const ReplaceRequest& req, EventSink& sink) {
    stats_.replaces_received++;
    ReplaceResult res;

    auto it = index_.find(req.original_client_id);
    if (MB_UNLIKELY(it == index_.end())) {
        res.reason = RejectReason::UnknownOrder;
        emit_reject(sink, req.original_client_id, res.reason);
        return res;
    }

    HotOrder* o = it->second;
    if (MB_UNLIKELY(o->participant != req.participant)) {
        res.reason = RejectReason::UnknownOrder;
        emit_reject(sink, req.original_client_id, res.reason);
        return res;
    }
    if (MB_UNLIKELY(is_terminal(static_cast<OrderStatus>(o->status)))) {
        res.reason = RejectReason::OrderAlreadyTerminal;
        emit_reject(sink, req.original_client_id, res.reason);
        return res;
    }

    const Side    side      = static_cast<Side>(o->side);
    const OrdType type      = static_cast<OrdType>(o->type);
    const Price   old_price = o->price;
    const Qty     already   = o->filled();
    const auto    old_idx   = static_cast<std::size_t>(o->level_index);

    const bool price_unchanged = (req.new_price == kNoPrice || req.new_price == old_price);
    if (price_unchanged && req.new_quantity > already && req.new_quantity <= o->quantity) {
        const Qty delta = o->quantity - req.new_quantity;
        o->quantity   = req.new_quantity;
        o->remaining -= delta;
        if (o->level_index >= 0 && old_idx < n_levels_) levels(side)[old_idx].reduce(delta);

        res.accepted      = true;
        res.priority_kept = true;
        res.remaining     = o->remaining;
        res.new_order_id  = o->id;
        stats_.replaces_accepted++;
        emit_ack(sink, req.original_client_id, static_cast<OrderStatus>(o->status));
        return res;
    }

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

bool V3Book::best_bid(Price& out) const {
    if (best_bid_idx_ == kNoIdx) return false;
    out = index_to_price(best_bid_idx_);
    return true;
}

bool V3Book::best_ask(Price& out) const {
    if (best_ask_idx_ == kNoIdx) return false;
    out = index_to_price(best_ask_idx_);
    return true;
}

BookSnapshot V3Book::snapshot(std::size_t depth) const {
    BookSnapshot snap;
    snap.seq = seq_;

    if (best_bid_idx_ != kNoIdx) {
        std::size_t idx = best_bid_idx_;
        for (std::size_t n = 0; n < depth; ++n) {
            const HotLevel& lvl = bid_levels_[idx];
            snap.bids.push_back(LevelView{index_to_price(idx), lvl.total_qty, lvl.count});
            std::size_t prv = 0;
            if (!bid_map_.find_prev(idx, prv)) break;
            idx = prv;
        }
    }
    if (best_ask_idx_ != kNoIdx) {
        std::size_t idx = best_ask_idx_;
        for (std::size_t n = 0; n < depth; ++n) {
            const HotLevel& lvl = ask_levels_[idx];
            snap.asks.push_back(LevelView{index_to_price(idx), lvl.total_qty, lvl.count});
            std::size_t nxt = 0;
            if (!ask_map_.find_next(idx, nxt)) break;
            idx = nxt;
        }
    }
    return snap;
}

bool V3Book::check_invariants(std::string& detail) const {
    Price bb = 0, ba = 0;
    if (best_bid(bb) && best_ask(ba) && bb >= ba) {
        detail += "crossed book: best_bid=" + std::to_string(bb) +
                  " >= best_ask=" + std::to_string(ba) + "\n";
        return false;
    }

    // The cached top-of-book indices are derived state, and derived state is
    // where bugs hide. Recompute both from the bitmap and compare.
    {
        std::size_t idx = 0;
        const std::size_t bid_scan = bid_map_.find_last(idx) ? idx : kNoIdx;
        if (bid_scan != best_bid_idx_) {
            detail += "cached best_bid_idx is stale\n";
            return false;
        }
        const std::size_t ask_scan = ask_map_.find_first(idx) ? idx : kNoIdx;
        if (ask_scan != best_ask_idx_) {
            detail += "cached best_ask_idx is stale\n";
            return false;
        }
    }

    std::size_t counted = 0;
    auto check_side = [&](const std::vector<HotLevel>& lv, const LevelBitmap& bm,
                          const char* name) -> bool {
        for (std::size_t i = 0; i < n_levels_; ++i) {
            const HotLevel& lvl = lv[i];
            const bool occupied = bm.test(i);
            if (occupied != !lvl.empty()) {
                detail += std::string(name) + " level " + std::to_string(i) +
                          ": bitmap/list disagreement\n";
                return false;
            }
            if (!occupied) continue;
            if (lvl.walk_qty() != lvl.total_qty) {
                detail += std::string(name) + " level " + std::to_string(i) +
                          " aggregate mismatch\n";
                return false;
            }
            if (lvl.walk_count() != lvl.count) {
                detail += std::string(name) + " level " + std::to_string(i) +
                          " count mismatch\n";
                return false;
            }
            counted += lvl.count;
        }
        return true;
    };

    if (!check_side(bid_levels_, bid_map_, "bid")) return false;
    if (!check_side(ask_levels_, ask_map_, "ask")) return false;

    if (counted != index_.size()) {
        detail += "resting count != index size\n";
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

std::string V3Book::debug_dump() const {
    std::ostringstream os;
    os << "=== V3 book dump: " << cfg_.symbol << " ===\n";
    os << "seq=" << seq_ << " resting=" << index_.size()
       << " pool_in_use=" << pool_.in_use() << "/" << pool_.capacity()
       << " tick_shift=" << tick_shift_
       << " best_bid_idx=" << (best_bid_idx_ == kNoIdx ? -1 : static_cast<long>(best_bid_idx_))
       << " best_ask_idx=" << (best_ask_idx_ == kNoIdx ? -1 : static_cast<long>(best_ask_idx_))
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
    return std::make_unique<V3Book>(cfg);
}

}  // namespace matchbook::v3
