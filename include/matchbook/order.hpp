#pragma once

// The resting-order record.
//
// One struct serves all four engine versions. V0 stores these by value inside a
// std::list; V1-V3 allocate them from a pool and thread them onto intrusive
// linked lists using the prev/next members below.

#include <cstddef>

#include "matchbook/types.hpp"

namespace matchbook {

// An "intrusive" list stores its next/prev pointers *inside* the element rather
// than in a separate node the container allocates. In Python terms: instead of
// a list object holding references to your objects, each object holds the links
// itself.
//
// Two consequences that matter here:
//   1. Enqueueing an order allocates nothing. std::list allocates a node per
//      push_back; an intrusive list does not.
//   2. Unlinking is O(1) from the element itself. Cancelling an order needs
//      only a pointer to the Order, not an iterator into some container.
//
// The cost is that an Order can be in at most one such list at a time, which is
// exactly true here: an order rests at one price level or nowhere.
struct Order {
    // --- identity -------------------------------------------------------
    ClientOrderId client_id = 0;
    OrderId       id        = kInvalidOrderId;
    ParticipantId participant = 0;

    // --- economics ------------------------------------------------------
    Price price    = 0;
    Qty   quantity = 0;  // original submitted quantity
    Qty   filled   = 0;  // cumulative filled; remaining() = quantity - filled

    // --- classification -------------------------------------------------
    Side        side   = Side::Buy;
    OrdType     type   = OrdType::Limit;
    OrderStatus status = OrderStatus::New;

    // --- bookkeeping ----------------------------------------------------
    SeqNum    seq       = 0;  // arrival sequence: the "time" in price-time priority
    Timestamp entry_ts  = 0;

    // --- intrusive list links (unused by V0) ----------------------------
    Order* prev = nullptr;
    Order* next = nullptr;

    // Index of the price level this order rests in, or -1 when not resting.
    // An index rather than a pointer so that V2/V3 can grow their level array
    // without invalidating every order's back-reference.
    std::int32_t level_index = -1;

    // Pool slot, used by the free list to recycle this record. -1 for V0.
    std::int32_t pool_slot = -1;

    [[nodiscard]] constexpr Qty remaining() const noexcept {
        return quantity - filled;
    }

    [[nodiscard]] constexpr bool is_resting() const noexcept {
        return status == OrderStatus::New || status == OrderStatus::PartiallyFilled;
    }

    void reset() noexcept {
        client_id   = 0;
        id          = kInvalidOrderId;
        participant = 0;
        price       = 0;
        quantity    = 0;
        filled      = 0;
        side        = Side::Buy;
        type        = OrdType::Limit;
        status      = OrderStatus::New;
        seq         = 0;
        entry_ts    = 0;
        prev        = nullptr;
        next        = nullptr;
        level_index = -1;
        // pool_slot deliberately preserved: it identifies this record's slot
        // for the lifetime of the pool, not the lifetime of the order in it.
    }
};

// A new-order request as it arrives from a participant, before the engine has
// assigned it an id or a sequence number.
struct NewOrder {
    ClientOrderId client_id   = 0;
    ParticipantId participant = 0;
    Side          side        = Side::Buy;
    OrdType       type        = OrdType::Limit;
    Price         price       = kNoPrice;  // kNoPrice for MARKET
    Qty           quantity    = 0;
};

// A replace (amend) request. Matchbook implements replace as cancel-then-new:
// any change to price or an increase in quantity loses time priority. A pure
// quantity *decrease* keeps priority, which is the near-universal convention.
struct ReplaceRequest {
    ClientOrderId original_client_id = 0;
    ClientOrderId new_client_id      = 0;
    ParticipantId participant        = 0;
    Price         new_price          = kNoPrice;
    Qty           new_quantity       = 0;
};

}  // namespace matchbook
