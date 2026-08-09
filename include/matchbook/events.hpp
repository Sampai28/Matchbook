#pragma once

// Engine output events and their canonical serialization.
//
// The serialization here is load-bearing for two separate guarantees:
//
//   * Deterministic replay: the same input stream must produce a byte-identical
//     event log across runs and across all four engine versions.
//   * The differential oracle: tools/reference_matcher.py emits this exact
//     format, so a Python reference and a C++ engine can be diffed literally
//     line by line.
//
// Any change to the format must be mirrored in reference_matcher.py and
// validate_fills.py, or the oracle silently stops comparing like with like.

#include <cstdio>
#include <string>
#include <vector>

#include "matchbook/types.hpp"

namespace matchbook {

enum class EventKind : std::uint8_t {
    Ack    = 0,  // order accepted (may or may not have rested)
    Fill   = 1,
    Cancel = 2,
    Reject = 3,
};

struct Event {
    EventKind     kind = EventKind::Ack;
    SeqNum        seq  = 0;

    // For Fill: the aggressing order. For everything else: the subject order.
    ClientOrderId client_id = 0;
    // Fill only: the resting order that provided the liquidity.
    ClientOrderId maker_client_id = 0;

    Price        price     = 0;
    Qty          quantity  = 0;
    Side         side      = Side::Buy;
    OrderStatus  status    = OrderStatus::New;
    RejectReason reason    = RejectReason::None;
};

// Canonical one-line-per-event text form.
//
// Fixed field order, no floating point, no locale-dependent formatting, no
// timestamps. Timestamps are excluded on purpose: they are the one field that
// legitimately differs between two correct runs, and including them would make
// byte-identical replay impossible to assert.
//
//   A,<seq>,<client_id>,<status>
//   F,<seq>,<taker_client_id>,<maker_client_id>,<price>,<qty>,<taker_side>
//   X,<seq>,<client_id>
//   R,<seq>,<client_id>,<reason>
inline void format_event(const Event& e, std::string& out) {
    char buf[192];
    int n = 0;
    switch (e.kind) {
        case EventKind::Ack:
            n = std::snprintf(buf, sizeof(buf), "A,%llu,%llu,%s\n",
                              static_cast<unsigned long long>(e.seq),
                              static_cast<unsigned long long>(e.client_id),
                              to_string(e.status).data());
            break;
        case EventKind::Fill:
            n = std::snprintf(buf, sizeof(buf), "F,%llu,%llu,%llu,%lld,%lld,%s\n",
                              static_cast<unsigned long long>(e.seq),
                              static_cast<unsigned long long>(e.client_id),
                              static_cast<unsigned long long>(e.maker_client_id),
                              static_cast<long long>(e.price),
                              static_cast<long long>(e.quantity),
                              to_string(e.side).data());
            break;
        case EventKind::Cancel:
            n = std::snprintf(buf, sizeof(buf), "X,%llu,%llu\n",
                              static_cast<unsigned long long>(e.seq),
                              static_cast<unsigned long long>(e.client_id));
            break;
        case EventKind::Reject:
            n = std::snprintf(buf, sizeof(buf), "R,%llu,%llu,%s\n",
                              static_cast<unsigned long long>(e.seq),
                              static_cast<unsigned long long>(e.client_id),
                              to_string(e.reason).data());
            break;
    }
    if (n > 0) out.append(buf, static_cast<std::size_t>(n));
}

inline std::string format_event(const Event& e) {
    std::string s;
    format_event(e, s);
    return s;
}

// Collects events for one submit/cancel/replace call.
//
// A plain vector reused across calls: clear() keeps the capacity, so steady
// state performs no allocation. Passing a sink in rather than returning a
// vector by value is what makes that reuse possible.
class EventSink {
public:
    explicit EventSink(std::size_t reserve = 64) { events_.reserve(reserve); }

    void clear() noexcept { events_.clear(); }
    void push(const Event& e) { events_.push_back(e); }

    [[nodiscard]] const std::vector<Event>& events() const noexcept { return events_; }
    [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }
    [[nodiscard]] bool empty() const noexcept { return events_.empty(); }

    [[nodiscard]] std::string to_text() const {
        std::string out;
        out.reserve(events_.size() * 48);
        for (const Event& e : events_) format_event(e, out);
        return out;
    }

private:
    std::vector<Event> events_;
};

}  // namespace matchbook
