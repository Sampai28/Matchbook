#pragma once

// Core scalar types and enumerations shared by every engine version.
//
// Everything here is a plain value type with no allocation and no virtual
// dispatch, so it can sit in the hot path of any implementation.

#include <cstdint>
#include <limits>
#include <string_view>

namespace matchbook {

// Prices are integers, never floating point. A price of 100.25 with a tick size
// of 0.01 is stored as 10025. Binary floating point cannot represent most
// decimal prices exactly, and an exchange that mis-compares two prices by one
// ULP has a correctness bug, not a rounding bug.
using Price = std::int64_t;

using Qty           = std::int64_t;
using OrderId       = std::uint64_t;  // internal, engine-assigned, dense
using ClientOrderId = std::uint64_t;  // external, participant-assigned
using ParticipantId = std::uint32_t;
using SeqNum        = std::uint64_t;
using Timestamp     = std::uint64_t;  // nanoseconds since engine start

// A market order carries no price. Using a sentinel rather than an optional
// keeps the Order struct trivially copyable and one word smaller.
inline constexpr Price kNoPrice      = std::numeric_limits<Price>::min();
inline constexpr Price kMaxPrice     = std::numeric_limits<Price>::max() / 4;
inline constexpr Qty   kMaxQty       = std::numeric_limits<Qty>::max() / 4;
inline constexpr OrderId kInvalidOrderId = 0;

enum class Side : std::uint8_t {
    Buy  = 0,
    Sell = 1,
};

constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

constexpr std::string_view to_string(Side s) noexcept {
    return s == Side::Buy ? "BUY" : "SELL";
}

// Order types. GTC limit orders rest; everything else either fills immediately
// or leaves.
enum class OrdType : std::uint8_t {
    Limit    = 0,  // GTC: match what crosses, rest the remainder
    Market   = 1,  // match at any price, cancel the remainder
    IOC      = 2,  // immediate-or-cancel: match what crosses, cancel remainder
    FOK      = 3,  // fill-or-kill: all of it immediately, or nothing at all
    PostOnly = 4,  // must rest; rejected outright if it would cross
};

constexpr std::string_view to_string(OrdType t) noexcept {
    switch (t) {
        case OrdType::Limit:    return "LIMIT";
        case OrdType::Market:   return "MARKET";
        case OrdType::IOC:      return "IOC";
        case OrdType::FOK:      return "FOK";
        case OrdType::PostOnly: return "POST_ONLY";
    }
    return "UNKNOWN";
}

enum class OrderStatus : std::uint8_t {
    New             = 0,
    PartiallyFilled = 1,
    Filled          = 2,  // terminal
    Cancelled       = 3,  // terminal
    Rejected        = 4,  // terminal
};

constexpr bool is_terminal(OrderStatus s) noexcept {
    return s == OrderStatus::Filled || s == OrderStatus::Cancelled ||
           s == OrderStatus::Rejected;
}

constexpr std::string_view to_string(OrderStatus s) noexcept {
    switch (s) {
        case OrderStatus::New:             return "NEW";
        case OrderStatus::PartiallyFilled: return "PARTIAL";
        case OrderStatus::Filled:          return "FILLED";
        case OrderStatus::Cancelled:       return "CANCELLED";
        case OrderStatus::Rejected:        return "REJECTED";
    }
    return "UNKNOWN";
}

// Every rejection path in the system has exactly one of these. Each maps to a
// counter exported by /stats, so "the engine rejected something" is never an
// unattributable event.
enum class RejectReason : std::uint16_t {
    None = 0,

    // --- inbound validation ---
    InvalidPrice,        // non-positive, or beyond kMaxPrice
    InvalidQuantity,     // non-positive, or beyond kMaxQty
    PriceNotOnTick,      // not an integral multiple of tick size
    PriceBandViolation,  // outside the configured band around reference
    UnknownSymbol,
    DuplicateClientOrderId,
    UnknownOrder,        // cancel/replace of an ID the book never saw
    OrderAlreadyTerminal,  // cancel/replace of a filled/cancelled/rejected order
    SelfTradePrevented,
    MarketOrderWithPrice,  // a MARKET order carrying a price is a client bug
    LimitOrderWithoutPrice,

    // --- order-type semantics ---
    PostOnlyWouldCross,  // POST_ONLY that would have taken liquidity
    FokUnfillable,       // FOK whose full quantity was not available
    NoLiquidity,         // MARKET/IOC against an empty opposite side

    // --- capacity ---
    BookFull,            // order pool exhausted
    PriceOutsideArray,   // V2/V3: price maps outside the flat array

    Count_  // keep last: array sizing
};

inline constexpr std::size_t kRejectReasonCount =
    static_cast<std::size_t>(RejectReason::Count_);

constexpr std::string_view to_string(RejectReason r) noexcept {
    switch (r) {
        case RejectReason::None:                   return "NONE";
        case RejectReason::InvalidPrice:           return "INVALID_PRICE";
        case RejectReason::InvalidQuantity:        return "INVALID_QUANTITY";
        case RejectReason::PriceNotOnTick:         return "PRICE_NOT_ON_TICK";
        case RejectReason::PriceBandViolation:     return "PRICE_BAND_VIOLATION";
        case RejectReason::UnknownSymbol:          return "UNKNOWN_SYMBOL";
        case RejectReason::DuplicateClientOrderId: return "DUPLICATE_CLIENT_ORDER_ID";
        case RejectReason::UnknownOrder:           return "UNKNOWN_ORDER";
        case RejectReason::OrderAlreadyTerminal:   return "ORDER_ALREADY_TERMINAL";
        case RejectReason::SelfTradePrevented:     return "SELF_TRADE_PREVENTED";
        case RejectReason::MarketOrderWithPrice:   return "MARKET_ORDER_WITH_PRICE";
        case RejectReason::LimitOrderWithoutPrice: return "LIMIT_ORDER_WITHOUT_PRICE";
        case RejectReason::PostOnlyWouldCross:     return "POST_ONLY_WOULD_CROSS";
        case RejectReason::FokUnfillable:          return "FOK_UNFILLABLE";
        case RejectReason::NoLiquidity:            return "NO_LIQUIDITY";
        case RejectReason::BookFull:               return "BOOK_FULL";
        case RejectReason::PriceOutsideArray:      return "PRICE_OUTSIDE_ARRAY";
        case RejectReason::Count_:                 return "INVALID";
    }
    return "UNKNOWN";
}

// Which engine implementation to instantiate. Selectable at runtime so the
// differential oracle and benchmark harness can drive all four from one binary.
enum class EngineVersion : std::uint8_t {
    V0_Naive = 0,
    V1_Pool  = 1,
    V2_Flat  = 2,
    V3_Tuned = 3,
};

constexpr std::string_view to_string(EngineVersion v) noexcept {
    switch (v) {
        case EngineVersion::V0_Naive: return "v0";
        case EngineVersion::V1_Pool:  return "v1";
        case EngineVersion::V2_Flat:  return "v2";
        case EngineVersion::V3_Tuned: return "v3";
    }
    return "unknown";
}

// Returns false rather than throwing on an unrecognised name; callers turn that
// into a usage message.
bool parse_engine_version(std::string_view name, EngineVersion& out) noexcept;

// Self-trade prevention policy.
//
// Only one policy is implemented (CancelResting). The enum exists so the choice
// is explicit at the call site and so adding CancelAggressor later does not
// change any signature.
enum class StpPolicy : std::uint8_t {
    CancelResting = 0,  // cancel the participant's own resting order, keep matching
};

}  // namespace matchbook
