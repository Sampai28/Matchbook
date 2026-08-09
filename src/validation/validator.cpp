#include "validator.hpp"

namespace matchbook {
namespace {

// Overflow-safe: checks the multiplication would not exceed the representable
// range *before* performing it. Doing the multiply first and testing the result
// is undefined behaviour on signed overflow, which the optimiser is entitled to
// assume never happens -- a class of bug that disappears in release builds and
// reappears under a different compiler.
[[nodiscard]] bool notional_would_overflow(Price price, Qty qty) noexcept {
    if (price <= 0 || qty <= 0) return false;  // caught by the range checks
    return price > kMaxPrice / qty;
}

}  // namespace

RejectReason Validator::validate_new(const NewOrder& req) const noexcept {
    // --- quantity -------------------------------------------------------
    if (req.quantity <= 0)      return RejectReason::InvalidQuantity;
    if (req.quantity > kMaxQty) return RejectReason::InvalidQuantity;

    // --- price, by order type -------------------------------------------
    if (req.type == OrdType::Market) {
        // A market order with a price is a client that has confused itself
        // about which field it is filling in. Rejecting is safer than silently
        // ignoring the price.
        if (req.price != kNoPrice) return RejectReason::MarketOrderWithPrice;
        return RejectReason::None;
    }

    if (req.price == kNoPrice)  return RejectReason::LimitOrderWithoutPrice;
    if (req.price <= 0)         return RejectReason::InvalidPrice;
    if (req.price > kMaxPrice)  return RejectReason::InvalidPrice;

    if (!cfg_.price_on_tick(req.price)) return RejectReason::PriceNotOnTick;
    if (!cfg_.price_in_band(req.price)) return RejectReason::PriceBandViolation;

    if (notional_would_overflow(req.price, req.quantity)) {
        return RejectReason::InvalidQuantity;
    }

    return RejectReason::None;
}

RejectReason Validator::validate_replace(const ReplaceRequest& req) const noexcept {
    if (req.new_quantity <= 0)      return RejectReason::InvalidQuantity;
    if (req.new_quantity > kMaxQty) return RejectReason::InvalidQuantity;

    // kNoPrice on a replace means "leave the price alone", which is legal.
    if (req.new_price != kNoPrice) {
        if (req.new_price <= 0)        return RejectReason::InvalidPrice;
        if (req.new_price > kMaxPrice) return RejectReason::InvalidPrice;
        if (!cfg_.price_on_tick(req.new_price)) return RejectReason::PriceNotOnTick;
        if (!cfg_.price_in_band(req.new_price)) return RejectReason::PriceBandViolation;
        if (notional_would_overflow(req.new_price, req.new_quantity)) {
            return RejectReason::InvalidQuantity;
        }
    }
    return RejectReason::None;
}

}  // namespace matchbook
