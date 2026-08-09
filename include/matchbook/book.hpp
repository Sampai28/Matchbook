#pragma once

// The stable interface every engine version implements.
//
// V0 through V3 differ enormously inside, and not at all here. That is the
// point: the differential oracle, the unit tests, the server and the benchmark
// harness all speak to IBook, so swapping implementations changes performance
// and nothing else. If an optimisation required changing this file, it would be
// changing behaviour, not just speed.

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "matchbook/config.hpp"
#include "matchbook/events.hpp"
#include "matchbook/metrics.hpp"
#include "matchbook/order.hpp"
#include "matchbook/types.hpp"

namespace matchbook {

struct SubmitResult {
    bool          accepted   = false;
    RejectReason  reason     = RejectReason::None;
    OrderId       order_id   = kInvalidOrderId;
    Qty           filled_qty = 0;
    Qty           remaining  = 0;
    bool          resting    = false;  // true if the remainder joined the book
    OrderStatus   status     = OrderStatus::New;
};

struct CancelResult {
    bool         accepted = false;
    RejectReason reason   = RejectReason::None;
    Qty          cancelled_qty = 0;
};

struct ReplaceResult {
    bool         accepted = false;
    RejectReason reason   = RejectReason::None;
    OrderId      new_order_id = kInvalidOrderId;
    Qty          filled_qty   = 0;
    Qty          remaining    = 0;
    bool         resting      = false;
    // True when the amend preserved time priority (a pure quantity decrease).
    bool         priority_kept = false;
};

// One aggregated price level, for depth queries and the ladder viewer.
struct LevelView {
    Price         price       = 0;
    Qty           quantity    = 0;  // sum of remaining() across resting orders
    std::uint32_t order_count = 0;
};

struct BookSnapshot {
    std::vector<LevelView> bids;  // descending price: bids[0] is the best bid
    std::vector<LevelView> asks;  // ascending price: asks[0] is the best ask
    SeqNum                 seq = 0;
};

// The engine interface.
//
// `virtual` here costs one indirect call per operation. That is deliberate: the
// benchmark measures a realistic deployment where the implementation is chosen
// at runtime, and paying the same vtable cost in all four versions keeps the
// comparison honest. V3's hot-path work happens *inside* submit(), below the
// virtual boundary, where the real cost is.
class IBook {
public:
    virtual ~IBook() = default;

    // Non-copyable, non-movable: engines own pools whose addresses are
    // referenced by intrusive pointers throughout the book. Copying one would
    // produce a book full of pointers into the original's memory.
    IBook(const IBook&)            = delete;
    IBook& operator=(const IBook&) = delete;

    // Submit a new order. Events produced (acks, fills, rejects) are appended
    // to `sink`, which the caller clears between calls.
    virtual SubmitResult submit(const NewOrder& req, EventSink& sink) = 0;

    // Cancel a resting order. `participant` must match the order's owner.
    virtual CancelResult cancel(ClientOrderId id, ParticipantId participant,
                                EventSink& sink) = 0;

    // Amend a resting order.
    virtual ReplaceResult replace(const ReplaceRequest& req, EventSink& sink) = 0;

    // --- read-only queries ------------------------------------------------
    [[nodiscard]] virtual BookSnapshot snapshot(std::size_t depth) const = 0;
    [[nodiscard]] virtual bool  best_bid(Price& out) const = 0;
    [[nodiscard]] virtual bool  best_ask(Price& out) const = 0;
    [[nodiscard]] virtual std::size_t resting_count() const = 0;
    [[nodiscard]] virtual const Stats& stats() const = 0;
    [[nodiscard]] virtual const BookConfig& config() const = 0;
    [[nodiscard]] virtual EngineVersion version() const = 0;

    // Full invariant sweep. Returns true when the book is consistent; on
    // failure, appends a human-readable description to `detail`. Called by the
    // invariant checker and directly by tests.
    [[nodiscard]] virtual bool check_invariants(std::string& detail) const = 0;

    // Human-readable dump used by paranoid-mode aborts and by test failures.
    [[nodiscard]] virtual std::string debug_dump() const = 0;

protected:
    IBook() = default;
};

// Constructs the requested implementation. Defined in src/engine/factory.cpp.
std::unique_ptr<IBook> make_book(EngineVersion version, const BookConfig& cfg);

}  // namespace matchbook
