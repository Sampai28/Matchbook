// The multi-symbol front door.
//
// Order of operations for every inbound request:
//   1. resolve the symbol            -> UnknownSymbol
//   2. stateless validation gate     -> InvalidPrice, PriceNotOnTick, ...
//   3. dispatch to the symbol's book -> DuplicateClientOrderId, matching, ...
//   4. invariant check per the book's configured mode
//
// Step 2 before step 3 is deliberate: a malformed request should never reach
// the matching engine at all, so the engines can assume their inputs are
// structurally sane and spend their branches on real work.

#include "matchbook/engine.hpp"

#include <string>

#include "validation/invariants.hpp"
#include "validation/validator.hpp"

namespace matchbook {

Engine::Engine(EngineVersion version) : version_(version) {}
Engine::~Engine() = default;

bool Engine::add_symbol(const BookConfig& cfg) {
    if (books_.find(cfg.symbol) != books_.end()) return false;

    SymbolEntry entry;
    entry.cfg        = cfg;
    entry.book       = make_book(version_, cfg);
    entry.validator  = std::make_unique<Validator>(cfg);
    entry.invariants = std::make_unique<InvariantChecker>(cfg.invariant_mode,
                                                          cfg.invariant_sample_period);
    books_.emplace(cfg.symbol, std::move(entry));
    return true;
}

Engine::SymbolEntry* Engine::find(std::string_view symbol) {
    // Heterogeneous lookup on unordered_map needs a transparent hash, which is
    // C++20 but not enabled by default. A temporary string is constructed here
    // instead; the cost falls on the cold path since the server resolves the
    // symbol once per request, not once per order in a batch.
    auto it = books_.find(std::string(symbol));
    return it == books_.end() ? nullptr : &it->second;
}

const Engine::SymbolEntry* Engine::find(std::string_view symbol) const {
    auto it = books_.find(std::string(symbol));
    return it == books_.end() ? nullptr : &it->second;
}

IBook* Engine::book(std::string_view symbol) {
    SymbolEntry* e = find(symbol);
    return e == nullptr ? nullptr : e->book.get();
}

const IBook* Engine::book(std::string_view symbol) const {
    const SymbolEntry* e = find(symbol);
    return e == nullptr ? nullptr : e->book.get();
}

std::vector<std::string> Engine::symbols() const {
    std::vector<std::string> out;
    out.reserve(books_.size());
    for (const auto& [name, entry] : books_) out.push_back(name);
    return out;
}

SubmitResult Engine::submit(const OrderRequest& req, EventSink& sink) {
    SubmitResult res;

    SymbolEntry* entry = find(req.symbol);
    if (entry == nullptr) {
        res.reason = RejectReason::UnknownSymbol;
        gate_stats_.count_reject(res.reason);
        Event e;
        e.kind      = EventKind::Reject;
        e.client_id = req.client_id;
        e.reason    = res.reason;
        e.status    = OrderStatus::Rejected;
        sink.push(e);
        return res;
    }

    NewOrder order;
    order.client_id   = req.client_id;
    order.participant = req.participant;
    order.side        = req.side;
    order.type        = req.type;
    order.price       = req.price;
    order.quantity    = req.quantity;

    const RejectReason gate = entry->validator->validate_new(order);
    if (gate != RejectReason::None) {
        entry->validator->count(gate);
        res.reason = gate;
        Event e;
        e.kind      = EventKind::Reject;
        e.client_id = req.client_id;
        e.reason    = gate;
        e.status    = OrderStatus::Rejected;
        sink.push(e);
        return res;
    }

    res = entry->book->submit(order, sink);

    // Sequence monotonicity is a property of the emitted stream, so it is
    // checked over the events rather than over the book.
    for (const Event& e : sink.events()) {
        if (e.seq != 0) {
            Stats dummy = entry->book->stats();
            entry->invariants->observe_sequence(e.seq, dummy);
        }
    }
    {
        Stats dummy = entry->book->stats();
        entry->invariants->after_event(*entry->book, dummy);
    }
    return res;
}

CancelResult Engine::cancel(const CancelRequest& req, EventSink& sink) {
    CancelResult res;

    SymbolEntry* entry = find(req.symbol);
    if (entry == nullptr) {
        res.reason = RejectReason::UnknownSymbol;
        gate_stats_.count_reject(res.reason);
        Event e;
        e.kind      = EventKind::Reject;
        e.client_id = req.client_id;
        e.reason    = res.reason;
        e.status    = OrderStatus::Rejected;
        sink.push(e);
        return res;
    }

    res = entry->book->cancel(req.client_id, req.participant, sink);

    Stats dummy = entry->book->stats();
    entry->invariants->after_event(*entry->book, dummy);
    return res;
}

ReplaceResult Engine::replace(std::string_view symbol, const ReplaceRequest& req,
                              EventSink& sink) {
    ReplaceResult res;

    SymbolEntry* entry = find(symbol);
    if (entry == nullptr) {
        res.reason = RejectReason::UnknownSymbol;
        gate_stats_.count_reject(res.reason);
        Event e;
        e.kind      = EventKind::Reject;
        e.client_id = req.original_client_id;
        e.reason    = res.reason;
        e.status    = OrderStatus::Rejected;
        sink.push(e);
        return res;
    }

    const RejectReason gate = entry->validator->validate_replace(req);
    if (gate != RejectReason::None) {
        entry->validator->count(gate);
        res.reason = gate;
        Event e;
        e.kind      = EventKind::Reject;
        e.client_id = req.original_client_id;
        e.reason    = gate;
        e.status    = OrderStatus::Rejected;
        sink.push(e);
        return res;
    }

    res = entry->book->replace(req, sink);

    Stats dummy = entry->book->stats();
    entry->invariants->after_event(*entry->book, dummy);
    return res;
}

Stats Engine::aggregate_stats() const {
    Stats total = gate_stats_;
    for (const auto& [name, entry] : books_) {
        const Stats& s = entry.book->stats();
        total.orders_received   += s.orders_received;
        total.orders_accepted   += s.orders_accepted;
        total.orders_rejected   += s.orders_rejected;
        total.orders_resting    += s.orders_resting;
        total.cancels_received  += s.cancels_received;
        total.cancels_accepted  += s.cancels_accepted;
        total.replaces_received += s.replaces_received;
        total.replaces_accepted += s.replaces_accepted;
        total.fills             += s.fills;
        total.buy_filled_qty    += s.buy_filled_qty;
        total.sell_filled_qty   += s.sell_filled_qty;
        total.buy_notional      += s.buy_notional;
        total.sell_notional     += s.sell_notional;
        total.invariant_checks  += s.invariant_checks;

        const Stats& vs = entry.validator->stats();
        total.orders_rejected += vs.orders_rejected;

        for (std::size_t i = 0; i < kRejectReasonCount; ++i) {
            total.rejects[i] += s.rejects[i] + vs.rejects[i];
        }
        total.invariant_violations += entry.invariants->violations();
    }
    return total;
}

}  // namespace matchbook
