#pragma once

// Market data gateway: sequence-numbered snapshots and incremental deltas.
//
// The wire format is specified in docs/protocol.md; this header is the
// implementation of that spec and the two must be changed together.
//
// The gateway sits beside the engine rather than inside it. It learns about
// book changes by being told to publish after a mutation, then diffing the
// book's current state against the state it last published. That is
// deliberately not the fastest possible design — an engine that emitted level
// deltas directly would avoid the diff — but it keeps every engine version
// untouched, which is the whole premise of the four-implementation comparison.
// The diff cost is paid on the HTTP thread, never in the matching path.
//
// Threading: the gateway has its own mutex, separate from the engine's. A
// streaming thread holds the gateway lock only long enough to copy a queued
// message; it never holds it while writing to a socket, and never touches the
// engine lock at all.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "matchbook/book.hpp"
#include "matchbook/events.hpp"
#include "matchbook/types.hpp"

namespace matchbook::gateway {

// How many price levels per side the gateway tracks for diffing.
//
// Levels beyond this are invisible to the delta stream. 512 is chosen to exceed
// the 500-level cap the /book endpoint already clamps to, so anything a client
// can ask to see is something the gateway is tracking. A book whose active
// range is deeper than this would need the tracker widened; it is a bound, not
// an assumption that books are small.
inline constexpr std::size_t kTrackDepth = 512;

// Per-subscriber queue bound. A slow consumer is dropped rather than allowed to
// apply backpressure to the publisher — the publisher runs on the thread that
// just mutated the book, and blocking it would mean a browser on a bad
// connection could stall order entry for everyone. A dropped message becomes a
// sequence gap, which the client is required to detect and recover from, so
// dropping is a supported path rather than data loss.
inline constexpr std::size_t kSubscriberQueueLimit = 512;

struct LevelState {
    Qty           quantity = 0;
    std::uint32_t orders   = 0;

    bool operator==(const LevelState& o) const noexcept {
        return quantity == o.quantity && orders == o.orders;
    }
};

// One connected SSE client.
struct Subscriber {
    std::mutex              mu;
    std::condition_variable cv;
    std::deque<std::string> queue;
    bool                    closed  = false;
    std::uint64_t           dropped = 0;

    // Returns false if the message was dropped because the queue was full.
    bool offer(const std::string& frame);
    void close();
};

using SubscriberPtr = std::shared_ptr<Subscriber>;

// Tracks one symbol's last-published state and fans messages out to its
// subscribers.
class SymbolStream {
public:
    explicit SymbolStream(std::string symbol) : symbol_(std::move(symbol)) {}

    // Rebuild the tracked state from the book and return a snapshot frame.
    // Used both on a new subscription and by GET /snapshot.
    std::string build_snapshot(const IBook& book, std::size_t depth, SeqNum& seq_out);

    // Diff the book against last-published state, emit a delta if anything
    // changed, and fan it out. `sink` supplies the trades. Returns the number
    // of subscribers the delta reached.
    std::size_t publish_delta(const IBook& book, const EventSink& sink);

    // Emit a heartbeat carrying the current sequence number.
    void publish_heartbeat();

    SubscriberPtr subscribe();
    void          unsubscribe(const SubscriberPtr& sub);

    [[nodiscard]] std::size_t   subscriber_count() const;
    [[nodiscard]] SeqNum        seq() const noexcept { return seq_; }
    [[nodiscard]] std::uint64_t messages_sent() const noexcept { return messages_sent_; }
    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }

private:
    void reindex(const IBook& book, std::size_t depth);
    void fan_out(const std::string& frame);

    std::string symbol_;
    SeqNum      seq_ = 0;

    // Last published state, keyed by price. Ordered maps rather than the flat
    // arrays the engines use: the gateway is not on the hot path, and an
    // ordered map makes the diff a linear merge instead of a scan over every
    // representable price.
    std::map<Price, LevelState, std::greater<Price>> bids_;
    std::map<Price, LevelState>                      asks_;
    bool indexed_ = false;

    mutable std::mutex         mu_;
    std::vector<SubscriberPtr> subscribers_;

    std::uint64_t messages_sent_ = 0;
    std::uint64_t dropped_       = 0;
};

// The registry: one SymbolStream per symbol.
class Gateway {
public:
    SymbolStream* stream(const std::string& symbol);
    SymbolStream& ensure(const std::string& symbol);

    [[nodiscard]] std::size_t   total_subscribers() const;
    [[nodiscard]] std::uint64_t total_messages() const;
    [[nodiscard]] std::uint64_t total_dropped() const;

    // Called on the heartbeat timer for every symbol.
    void heartbeat_all();

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<SymbolStream>> streams_;
};

// --- frame construction, exposed for the delta-correctness test -------------

// Applies a delta frame's level entries to a tracked book state, exactly as a
// conforming client must. Used by tests/test_gateway_delta.cpp to assert that
// snapshot + replayed deltas equals the engine's real book.
struct TrackedBook {
    std::map<Price, LevelState, std::greater<Price>> bids;
    std::map<Price, LevelState>                      asks;

    void apply_level(Side side, Price price, Qty quantity, std::uint32_t orders);
    [[nodiscard]] bool equals(const IBook& book, std::size_t depth,
                              std::string& detail) const;
};

}  // namespace matchbook::gateway
