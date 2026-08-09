#pragma once

// The multi-symbol front door: a registry of books plus the inbound validation
// gate that runs before any of them is touched.
//
// Everything is single-threaded by design. There is no locking anywhere in
// Matchbook, and adding a second thread would require revisiting every engine.
// See "Known limitations" in the README.

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "matchbook/book.hpp"
#include "matchbook/config.hpp"
#include "matchbook/events.hpp"
#include "matchbook/types.hpp"

namespace matchbook {

class Validator;
class InvariantChecker;

// A request as it arrives at the engine, before symbol resolution.
struct OrderRequest {
    std::string   symbol;
    ClientOrderId client_id   = 0;
    ParticipantId participant = 0;
    Side          side        = Side::Buy;
    OrdType       type        = OrdType::Limit;
    Price         price       = kNoPrice;
    Qty           quantity    = 0;
};

struct CancelRequest {
    std::string   symbol;
    ClientOrderId client_id   = 0;
    ParticipantId participant = 0;
};

class Engine {
public:
    explicit Engine(EngineVersion version);
    ~Engine();

    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;

    // Registers a symbol and constructs its book. Returns false if the symbol
    // already exists.
    bool add_symbol(const BookConfig& cfg);

    [[nodiscard]] IBook*       book(std::string_view symbol);
    [[nodiscard]] const IBook* book(std::string_view symbol) const;
    [[nodiscard]] std::vector<std::string> symbols() const;

    // --- the three inbound operations -------------------------------------
    // Each runs the validation gate first, then dispatches to the symbol's
    // book, then runs the invariant checker according to the book's configured
    // mode.
    SubmitResult  submit(const OrderRequest& req, EventSink& sink);
    CancelResult  cancel(const CancelRequest& req, EventSink& sink);
    ReplaceResult replace(std::string_view symbol, const ReplaceRequest& req,
                          EventSink& sink);

    [[nodiscard]] EngineVersion version() const noexcept { return version_; }

    // Aggregated across every book, plus engine-level rejections that never
    // reached a book (unknown symbol, for instance).
    [[nodiscard]] Stats aggregate_stats() const;

    // Engine-level counters for requests rejected before symbol resolution.
    [[nodiscard]] const Stats& gate_stats() const noexcept { return gate_stats_; }

private:
    struct SymbolEntry {
        BookConfig             cfg;
        std::unique_ptr<IBook> book;
        std::unique_ptr<Validator> validator;
        std::unique_ptr<InvariantChecker> invariants;
    };

    SymbolEntry*       find(std::string_view symbol);
    const SymbolEntry* find(std::string_view symbol) const;

    EngineVersion version_;
    // Keyed by std::string rather than string_view: the map owns the key, and a
    // view into a caller's temporary would dangle.
    std::unordered_map<std::string, SymbolEntry> books_;
    Stats gate_stats_;
};

}  // namespace matchbook
