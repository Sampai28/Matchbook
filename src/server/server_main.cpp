// matchbook-server — HTTP front end over the engine.
//
// The engine has no locking anywhere and never will; it is single-threaded by
// design. The *server* is not, because it cannot be: an SSE stream holds its
// worker thread for the lifetime of the connection, so a one-worker pool would
// stop serving everything the moment one browser opened /stream. Instead the
// pool is wider and ServerState::engine_mu serialises every engine call. Do
// not remove that mutex, and do not call the engine without holding it.
//
//   POST   /order                     submit a new order
//   DELETE /order/{client_id}         cancel, ?symbol=&participant=
//   GET    /book/{symbol}?depth=N     depth-of-book ladder
//   GET    /stream/{symbol}           SSE: snapshot then incremental deltas
//   GET    /snapshot/{symbol}?depth=N one snapshot frame as plain JSON
//   GET    /stats                     counters, rejections, latency percentiles
//   GET    /stats/engine              stable shape for the UI stats panel
//   GET    /healthz                   liveness
//   GET    /                          static ladder viewer from web/

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <httplib.h>

#include "matchbook/engine.hpp"
#include "matchbook/metrics.hpp"
#include "matchbook/types.hpp"
#include "server/gateway.hpp"
#include "server/json.hpp"

namespace {

using namespace matchbook;

struct ServerOptions {
    std::string   host        = "0.0.0.0";
    int           port        = 8080;
    std::string   web_dir     = "web";
    EngineVersion version     = EngineVersion::V3_Tuned;
    std::string   symbols     = "MBK,ACME";
    Price         tick        = 1;
    Price         reference   = 100'000;
    InvariantMode invariants  = InvariantMode::Sampled;
};

// Latency of the engine call itself, not of the HTTP round trip. The HTTP layer
// is orders of magnitude slower than matching, so including it would drown the
// signal the /stats percentiles exist to show.
struct ServerState {
    std::unique_ptr<Engine> engine;
    LatencyHistogram submit_latency;
    LatencyHistogram cancel_latency;
    std::atomic<std::uint64_t> http_requests{0};
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    // The market data gateway, and the lock that makes a multi-worker server
    // safe against a lock-free engine.
    //
    // The engine is still single-threaded and still has no internal locking.
    // What changed is that the server can no longer be single-threaded: an SSE
    // connection holds its worker for the lifetime of the stream, so with the
    // original ThreadPool(1) the first browser to open /stream would have
    // frozen the entire server — no order entry, no snapshots, nothing.
    //
    // So the pool is wider and every engine call is serialised by this mutex.
    // Streaming threads take it only to copy a queued message; they never hold
    // it while writing to a socket, which is what keeps a slow client from
    // blocking matching.
    std::mutex        engine_mu;
    gateway::Gateway  gw;
};

bool parse_side(std::string_view s, Side& out) {
    if (s == "BUY"  || s == "buy"  || s == "B") { out = Side::Buy;  return true; }
    if (s == "SELL" || s == "sell" || s == "S") { out = Side::Sell; return true; }
    return false;
}

bool parse_type(std::string_view s, OrdType& out) {
    if (s == "LIMIT"     || s == "limit")     { out = OrdType::Limit;    return true; }
    if (s == "MARKET"    || s == "market")    { out = OrdType::Market;   return true; }
    if (s == "IOC"       || s == "ioc")       { out = OrdType::IOC;      return true; }
    if (s == "FOK"       || s == "fok")       { out = OrdType::FOK;      return true; }
    if (s == "POST_ONLY" || s == "post_only") { out = OrdType::PostOnly; return true; }
    return false;
}

std::string error_json(std::string_view code, std::string_view message) {
    json::Writer w;
    w.begin_object();
    w.field("ok", false);
    w.field("error", code);
    w.field("message", message);
    w.end_object();
    return w.str();
}

std::string events_json(const EventSink& sink) {
    json::Writer w;
    w.key("events");
    w.begin_array();
    for (const Event& e : sink.events()) {
        w.begin_object();
        switch (e.kind) {
            case EventKind::Ack:    w.field("kind", "ACK");    break;
            case EventKind::Fill:   w.field("kind", "FILL");   break;
            case EventKind::Cancel: w.field("kind", "CANCEL"); break;
            case EventKind::Reject: w.field("kind", "REJECT"); break;
        }
        w.field("seq", static_cast<std::uint64_t>(e.seq));
        w.field("clientOrderId", static_cast<std::uint64_t>(e.client_id));
        if (e.kind == EventKind::Fill) {
            w.field("makerClientOrderId", static_cast<std::uint64_t>(e.maker_client_id));
            w.field("price", static_cast<std::int64_t>(e.price));
            w.field("quantity", static_cast<std::int64_t>(e.quantity));
            w.field("takerSide", to_string(e.side));
        }
        if (e.kind == EventKind::Reject) w.field("reason", to_string(e.reason));
        w.field("status", to_string(e.status));
        w.end_object();
    }
    w.end_array();
    return w.str();
}

void handle_post_order(ServerState& st, const httplib::Request& req, httplib::Response& res) {
    json::Object body;
    std::string  parse_error;
    if (!json::parse_object(req.body, body, parse_error)) {
        res.status = 400;
        res.set_content(error_json("BAD_JSON", parse_error), "application/json");
        return;
    }

    OrderRequest o;
    o.symbol = body.get_string("symbol");
    if (o.symbol.empty()) {
        res.status = 400;
        res.set_content(error_json("MISSING_FIELD", "symbol is required"), "application/json");
        return;
    }

    std::int64_t v = 0;
    if (!body.get_int("clientOrderId", v) || v <= 0) {
        res.status = 400;
        res.set_content(error_json("MISSING_FIELD", "clientOrderId must be a positive integer"),
                        "application/json");
        return;
    }
    o.client_id = static_cast<ClientOrderId>(v);

    if (body.get_int("participant", v) && v >= 0) {
        o.participant = static_cast<ParticipantId>(v);
    }

    if (!parse_side(body.get_string("side"), o.side)) {
        res.status = 400;
        res.set_content(error_json("INVALID_FIELD", "side must be BUY or SELL"),
                        "application/json");
        return;
    }
    if (!parse_type(body.get_string("type", "LIMIT"), o.type)) {
        res.status = 400;
        res.set_content(error_json("INVALID_FIELD",
                                   "type must be LIMIT, MARKET, IOC, FOK or POST_ONLY"),
                        "application/json");
        return;
    }

    if (!body.get_int("quantity", v)) {
        res.status = 400;
        res.set_content(error_json("MISSING_FIELD", "quantity is required"), "application/json");
        return;
    }
    o.quantity = static_cast<Qty>(v);

    if (o.type == OrdType::Market) {
        o.price = kNoPrice;
    } else if (body.get_int("price", v)) {
        o.price = static_cast<Price>(v);
    } else {
        res.status = 400;
        res.set_content(error_json("MISSING_FIELD", "price is required for non-MARKET orders"),
                        "application/json");
        return;
    }

    EventSink sink;
    SubmitResult r;
    {
        std::lock_guard<std::mutex> lock(st.engine_mu);
        const auto t0 = std::chrono::steady_clock::now();
        r = st.engine->submit(o, sink);
        const auto t1 = std::chrono::steady_clock::now();
        st.submit_latency.record(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));

        // Publish inside the lock, so the diff sees the book in exactly the
        // state this operation left it. Outside the lock, a concurrent submit
        // could land first and both deltas would describe the same final
        // state — the sequence would advance twice for one change and a client
        // applying them would be correct by luck rather than by construction.
        if (const IBook* b = st.engine->book(o.symbol)) {
            st.gw.ensure(o.symbol).publish_delta(*b, sink);
        }
    }

    json::Writer w;
    w.begin_object();
    w.field("ok", r.accepted);
    w.field("orderId", static_cast<std::uint64_t>(r.order_id));
    w.field("status", to_string(r.status));
    w.field("filledQuantity", static_cast<std::int64_t>(r.filled_qty));
    w.field("remainingQuantity", static_cast<std::int64_t>(r.remaining));
    w.field("resting", r.resting);
    if (!r.accepted) w.field("reason", to_string(r.reason));
    w.end_object();

    // The events array is appended by splicing, since the Writer builds one
    // flat object and the events belong inside it.
    std::string out = w.str();
    out.pop_back();                       // drop the closing brace
    out += ',';
    out += events_json(sink);
    out += '}';

    res.status = r.accepted ? 200 : 422;
    res.set_content(out, "application/json");
}

void handle_delete_order(ServerState& st, const httplib::Request& req, httplib::Response& res) {
    CancelRequest c;
    c.symbol = req.get_param_value("symbol");
    if (c.symbol.empty()) {
        res.status = 400;
        res.set_content(error_json("MISSING_FIELD", "symbol query parameter is required"),
                        "application/json");
        return;
    }
    c.client_id = static_cast<ClientOrderId>(std::strtoull(req.matches[1].str().c_str(), nullptr, 10));
    if (req.has_param("participant")) {
        c.participant = static_cast<ParticipantId>(
            std::strtoul(req.get_param_value("participant").c_str(), nullptr, 10));
    }

    EventSink sink;
    CancelResult r;
    {
        std::lock_guard<std::mutex> lock(st.engine_mu);
        const auto t0 = std::chrono::steady_clock::now();
        r = st.engine->cancel(c, sink);
        const auto t1 = std::chrono::steady_clock::now();
        st.cancel_latency.record(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));

        if (const IBook* b = st.engine->book(c.symbol)) {
            st.gw.ensure(c.symbol).publish_delta(*b, sink);
        }
    }

    json::Writer w;
    w.begin_object();
    w.field("ok", r.accepted);
    w.field("cancelledQuantity", static_cast<std::int64_t>(r.cancelled_qty));
    if (!r.accepted) w.field("reason", to_string(r.reason));
    w.end_object();

    std::string out = w.str();
    out.pop_back();
    out += ',';
    out += events_json(sink);
    out += '}';

    res.status = r.accepted ? 200 : 404;
    res.set_content(out, "application/json");
}

void handle_get_book(ServerState& st, const httplib::Request& req, httplib::Response& res) {
    const std::string symbol = req.matches[1].str();
    // Held for the whole handler: the snapshot below walks the book, and a
    // concurrent submit part-way through would produce a ladder that never
    // existed at any instant.
    std::lock_guard<std::mutex> lock(st.engine_mu);
    const IBook* book = st.engine->book(symbol);
    if (book == nullptr) {
        res.status = 404;
        res.set_content(error_json("UNKNOWN_SYMBOL", "no such symbol"), "application/json");
        return;
    }

    std::size_t depth = 10;
    if (req.has_param("depth")) {
        const long d = std::strtol(req.get_param_value("depth").c_str(), nullptr, 10);
        // Clamped rather than rejected: a viewer asking for 10,000 levels is a
        // UI bug, not an attack, and a clamp keeps the page working.
        if (d > 0) depth = static_cast<std::size_t>(d > 500 ? 500 : d);
    }

    const BookSnapshot snap = book->snapshot(depth);

    json::Writer w;
    w.begin_object();
    w.field("symbol", symbol);
    w.field("engine", to_string(book->version()));
    w.field("seq", static_cast<std::uint64_t>(snap.seq));
    w.field("restingOrders", static_cast<std::uint64_t>(book->resting_count()));

    Price px = 0;
    w.key("bestBid");
    if (book->best_bid(px)) w.value(static_cast<std::int64_t>(px)); else w.null();
    w.key("bestAsk");
    if (book->best_ask(px)) w.value(static_cast<std::int64_t>(px)); else w.null();

    auto write_side = [&](std::string_view name, const std::vector<LevelView>& levels) {
        w.key(name);
        w.begin_array();
        for (const LevelView& l : levels) {
            w.begin_object();
            w.field("price", static_cast<std::int64_t>(l.price));
            w.field("quantity", static_cast<std::int64_t>(l.quantity));
            w.field("orders", static_cast<std::uint64_t>(l.order_count));
            w.end_object();
        }
        w.end_array();
    };
    write_side("bids", snap.bids);
    write_side("asks", snap.asks);
    w.end_object();

    res.set_content(w.str(), "application/json");
}

// --- market data gateway ---------------------------------------------------

std::size_t depth_param(const httplib::Request& req, std::size_t fallback) {
    if (!req.has_param("depth")) return fallback;
    const long d = std::strtol(req.get_param_value("depth").c_str(), nullptr, 10);
    if (d <= 0) return fallback;
    return static_cast<std::size_t>(d > 500 ? 500 : d);
}

void handle_snapshot(ServerState& st, const httplib::Request& req, httplib::Response& res) {
    const std::string symbol = req.matches[1].str();
    const std::size_t depth  = depth_param(req, 50);

    std::lock_guard<std::mutex> lock(st.engine_mu);
    const IBook* book = st.engine->book(symbol);
    if (book == nullptr) {
        res.status = 404;
        res.set_content(error_json("UNKNOWN_SYMBOL", "no such symbol"), "application/json");
        return;
    }
    SeqNum seq = 0;
    const std::string body = st.gw.ensure(symbol).build_snapshot(*book, depth, seq);
    res.set_content(body, "application/json");
}

void handle_stream(ServerState& st, const httplib::Request& req, httplib::Response& res) {
    const std::string symbol = req.matches[1].str();
    const std::size_t depth  = depth_param(req, 50);

    std::string initial;
    {
        std::lock_guard<std::mutex> lock(st.engine_mu);
        const IBook* book = st.engine->book(symbol);
        if (book == nullptr) {
            res.status = 404;
            res.set_content(error_json("UNKNOWN_SYMBOL", "no such symbol"),
                            "application/json");
            return;
        }
        SeqNum seq = 0;
        // The snapshot is built while holding the engine lock and *before* the
        // subscription exists, so there is no window in which a delta could be
        // published between the snapshot being taken and the subscriber being
        // registered. That window is exactly how a client ends up permanently
        // one delta behind with no gap to detect.
        initial = "event: snapshot\ndata: " +
                  st.gw.ensure(symbol).build_snapshot(*book, depth, seq) + "\n\n";
    }

    auto sub = st.gw.ensure(symbol).subscribe();
    auto* gw = &st.gw;
    auto  sym = symbol;

    res.set_header("Cache-Control", "no-cache");
    // Nginx and friends buffer proxied responses by default, which for SSE
    // means the client receives nothing until the buffer fills — i.e. never.
    res.set_header("X-Accel-Buffering", "no");

    bool sent_initial = false;

    res.set_chunked_content_provider(
        "text/event-stream",
        [sub, initial, &sent_initial](std::size_t /*offset*/,
                                      httplib::DataSink& sink) mutable -> bool {
            if (!sent_initial) {
                sent_initial = true;
                if (!sink.write(initial.data(), initial.size())) return false;
            }

            std::vector<std::string> batch;
            {
                std::unique_lock<std::mutex> lock(sub->mu);
                // Bounded wait rather than an indefinite one: it bounds how
                // long this thread takes to notice a closed subscriber, and it
                // is what makes the heartbeat timer able to reach an otherwise
                // idle stream.
                sub->cv.wait_for(lock, std::chrono::milliseconds(500), [&] {
                    return !sub->queue.empty() || sub->closed;
                });
                if (sub->closed && sub->queue.empty()) return false;
                while (!sub->queue.empty()) {
                    batch.push_back(std::move(sub->queue.front()));
                    sub->queue.pop_front();
                }
            }

            for (const std::string& frame : batch) {
                // A failed write means the peer is gone. Returning false ends
                // the provider, which is what releases this worker thread.
                if (!sink.write(frame.data(), frame.size())) return false;
            }
            return true;
        },
        [sub, gw, sym](bool) {
            // Releaser: runs when the connection ends for any reason. Without
            // it the subscriber stays registered forever and every future
            // publish fans out into a queue nobody drains.
            sub->close();
            if (gateway::SymbolStream* s = gw->stream(sym)) s->unsubscribe(sub);
        });
}

void handle_engine_stats(ServerState& st, const httplib::Request&, httplib::Response& res) {
    json::Writer w;
    w.begin_object();

    {
        std::lock_guard<std::mutex> lock(st.engine_mu);
        const Stats s = st.engine->aggregate_stats();
        w.field("engine", to_string(st.engine->version()));

        auto percentiles = [&w](std::string_view name, const LatencyHistogram& h) {
            w.key(name);
            w.begin_object();
            w.field("p50",  h.percentile(50.0));
            w.field("p99",  h.percentile(99.0));
            w.field("p999", h.percentile(99.9));
            w.field("count", h.count());
            w.end_object();
        };
        percentiles("submit", st.submit_latency);
        percentiles("cancel", st.cancel_latency);

        w.key("counters");
        w.begin_object();
        w.field("ordersAccepted", s.orders_accepted);
        w.field("ordersRejected", s.orders_rejected);
        w.field("ordersResting",  s.orders_resting);
        w.field("fills",          s.fills);
        w.end_object();
    }

    w.key("streams");
    w.begin_object();
    w.field("clients",      static_cast<std::uint64_t>(st.gw.total_subscribers()));
    w.field("messagesSent", st.gw.total_messages());
    // Non-zero means some client saw a gap and had to re-snapshot.
    w.field("dropped",      st.gw.total_dropped());
    w.end_object();

    w.end_object();
    res.set_content(w.str(), "application/json");
}

void handle_stats(ServerState& st, const httplib::Request&, httplib::Response& res) {
    // Held for the whole handler. The latency histograms read below are written
    // under this same lock by the submit and cancel paths, and LatencyHistogram
    // is not thread-safe — percentile() scans the bucket vector while record()
    // may be mutating it.
    std::lock_guard<std::mutex> lock(st.engine_mu);
    const Stats s = st.engine->aggregate_stats();

    json::Writer w;
    w.begin_object();
    w.field("engine", to_string(st.engine->version()));
    w.field("uptimeSeconds",
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - st.started).count()));
    // .load() rather than relying on the implicit conversion: with several
    // field() overloads, an atomic converting implicitly is ambiguous.
    w.field("httpRequests", st.http_requests.load());

    w.key("counters");
    w.begin_object();
    w.field("ordersReceived",   s.orders_received);
    w.field("ordersAccepted",   s.orders_accepted);
    w.field("ordersRejected",   s.orders_rejected);
    w.field("ordersResting",    s.orders_resting);
    w.field("cancelsReceived",  s.cancels_received);
    w.field("cancelsAccepted",  s.cancels_accepted);
    w.field("replacesReceived", s.replaces_received);
    w.field("replacesAccepted", s.replaces_accepted);
    w.field("fills",            s.fills);
    w.end_object();

    w.key("conservation");
    w.begin_object();
    w.field("buyFilledQuantity",  s.buy_filled_qty);
    w.field("sellFilledQuantity", s.sell_filled_qty);
    w.field("buyNotional",        s.buy_notional);
    w.field("sellNotional",       s.sell_notional);
    w.field("balanced", s.buy_filled_qty == s.sell_filled_qty &&
                        s.buy_notional == s.sell_notional);
    w.end_object();

    w.key("integrity");
    w.begin_object();
    w.field("invariantChecks",     s.invariant_checks);
    w.field("invariantViolations", s.invariant_violations);
    w.end_object();

    // Every rejection reason is emitted, including the zeroes. A reason that
    // appears only once it has fired is indistinguishable from a reason that
    // was never wired up.
    w.key("rejections");
    w.begin_object();
    for (std::size_t i = 1; i < kRejectReasonCount; ++i) {
        w.field(to_string(static_cast<RejectReason>(i)), s.rejects[i]);
    }
    w.end_object();

    auto write_hist = [&](std::string_view name, const LatencyHistogram& h) {
        w.key(name);
        w.begin_object();
        w.field("count", h.count());
        w.field("p50",   h.percentile(50.0));
        w.field("p90",   h.percentile(90.0));
        w.field("p99",   h.percentile(99.0));
        w.field("p999",  h.percentile(99.9));
        w.field("max",   h.max());
        w.key("mean"); w.value_double(h.mean());
        w.end_object();
    };
    w.key("latencyNanos");
    w.begin_object();
    write_hist("submit", st.submit_latency);
    write_hist("cancel", st.cancel_latency);
    w.end_object();

    // These are live measurements of this process, unlike the projections in
    // docs/expected-performance.md. They are still HTTP-path measurements taken
    // under whatever else the machine is doing, so they are not a substitute
    // for bench/.
    w.field("note",
            "latency figures are measured around the engine call in this process; "
            "for benchmark-quality numbers run bench/ natively under WSL2");
    w.end_object();

    res.set_content(w.str(), "application/json");
}

}  // namespace

int main(int argc, char** argv) {
    ServerOptions opt;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "matchbook-server: %s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };

        if (a == "--port")            opt.port = std::atoi(next("--port"));
        else if (a == "--host")       opt.host = next("--host");
        else if (a == "--web")        opt.web_dir = next("--web");
        else if (a == "--symbols")    opt.symbols = next("--symbols");
        else if (a == "--tick")       opt.tick = std::atoll(next("--tick"));
        else if (a == "--reference")  opt.reference = std::atoll(next("--reference"));
        else if (a == "--paranoid")   opt.invariants = InvariantMode::Paranoid;
        else if (a == "--no-invariants") opt.invariants = InvariantMode::Off;
        else if (a == "--engine") {
            if (!parse_engine_version(next("--engine"), opt.version)) {
                std::fprintf(stderr, "matchbook-server: unknown engine (use v0|v1|v2|v3)\n");
                return 2;
            }
        } else if (a == "--help" || a == "-h") {
            std::printf(
                "matchbook-server [options]\n"
                "  --engine v0|v1|v2|v3   engine implementation (default v3)\n"
                "  --port N               listen port (default 8080)\n"
                "  --host ADDR            bind address (default 0.0.0.0)\n"
                "  --symbols A,B,C        symbols to create (default MBK,ACME)\n"
                "  --tick N               tick size (default 1)\n"
                "  --reference N          reference price (default 100000)\n"
                "  --web DIR              static file directory (default web)\n"
                "  --paranoid             check invariants after every event\n"
                "  --no-invariants        disable invariant checking\n");
            return 0;
        } else {
            std::fprintf(stderr, "matchbook-server: unknown argument '%s' (try --help)\n", a.c_str());
            return 2;
        }
    }

    ServerState st;
    st.engine = std::make_unique<Engine>(opt.version);

    {
        std::string_view rest = opt.symbols;
        while (!rest.empty()) {
            const std::size_t comma = rest.find(',');
            const std::string_view name = rest.substr(0, comma);
            if (!name.empty()) {
                BookConfig cfg;
                cfg.symbol          = std::string(name);
                cfg.tick_size       = opt.tick;
                cfg.reference_price = opt.reference;
                cfg.invariant_mode  = opt.invariants;
                st.engine->add_symbol(cfg);
            }
            if (comma == std::string_view::npos) break;
            rest.remove_prefix(comma + 1);
        }
    }

    httplib::Server srv;

    // Eight workers, not one.
    //
    // The original single worker was correct when every request was
    // short-lived: the engine has no locks, so serialising requests by having
    // exactly one thread was the cheapest possible safety argument. SSE breaks
    // that argument, because a stream occupies its worker until the client
    // disconnects. With one worker, the first browser to open /stream would
    // take the whole server down with it.
    //
    // Safety now comes from ServerState::engine_mu instead, which every engine
    // call acquires. The engine itself is unchanged and still has no internal
    // locking. Streaming threads spend nearly all their time blocked on their
    // own condition variable, holding nothing.
    //
    // Sized for concurrent viewers plus order entry, not for throughput: the
    // engine is serialised regardless, so more threads buy connection capacity,
    // never parallel matching.
    srv.new_task_queue = [] { return new httplib::ThreadPool(8); };

    srv.set_pre_routing_handler([&st](const httplib::Request&, httplib::Response&) {
        st.http_requests++;
        return httplib::Server::HandlerResponse::Unhandled;
    });

    srv.Post("/order", [&st](const httplib::Request& q, httplib::Response& r) {
        handle_post_order(st, q, r);
    });
    srv.Delete(R"(/order/(\d+))", [&st](const httplib::Request& q, httplib::Response& r) {
        handle_delete_order(st, q, r);
    });
    srv.Get(R"(/book/([A-Za-z0-9_.\-]+))", [&st](const httplib::Request& q, httplib::Response& r) {
        handle_get_book(st, q, r);
    });
    srv.Get(R"(/stream/([A-Za-z0-9_.\-]+))", [&st](const httplib::Request& q, httplib::Response& r) {
        handle_stream(st, q, r);
    });
    srv.Get(R"(/snapshot/([A-Za-z0-9_.\-]+))", [&st](const httplib::Request& q, httplib::Response& r) {
        handle_snapshot(st, q, r);
    });
    // Registered before /stats so the more specific path wins; cpp-httplib
    // matches in registration order and "/stats" as a plain string would not
    // swallow "/stats/engine", but ordering it this way keeps that independent
    // of the matcher's behaviour.
    srv.Get("/stats/engine", [&st](const httplib::Request& q, httplib::Response& r) {
        handle_engine_stats(st, q, r);
    });
    srv.Get("/stats", [&st](const httplib::Request& q, httplib::Response& r) {
        handle_stats(st, q, r);
    });
    srv.Get("/healthz", [](const httplib::Request&, httplib::Response& r) {
        r.set_content("{\"status\":\"ok\"}", "application/json");
    });

    if (!srv.set_mount_point("/", opt.web_dir)) {
        std::fprintf(stderr,
                     "[matchbook] warning: web directory '%s' not found; "
                     "the ladder viewer will not be served\n", opt.web_dir.c_str());
    }

    std::printf("matchbook-server listening on %s:%d (engine %s, symbols %s)\n",
                opt.host.c_str(), opt.port,
                to_string(opt.version).data(), opt.symbols.c_str());
    std::printf("  ladder viewer : http://localhost:%d/\n", opt.port);
    std::printf("  stats         : http://localhost:%d/stats\n", opt.port);
    std::printf("  market data   : http://localhost:%d/stream/MBK\n", opt.port);
    std::fflush(stdout);

    // Heartbeat timer.
    //
    // Keeps idle streams alive through proxies that close quiet connections,
    // and lets a client tell "nothing is trading" apart from "the connection
    // died". It touches only the gateway, never the engine, so it needs no
    // lock and cannot stall matching.
    std::atomic<bool> running{true};
    std::thread heartbeat([&st, &running] {
        while (running.load(std::memory_order_relaxed)) {
            // Short sleeps rather than one long one, so shutdown is prompt
            // instead of waiting out a full heartbeat interval.
            for (int i = 0; i < 50 && running.load(std::memory_order_relaxed); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!running.load(std::memory_order_relaxed)) break;
            st.gw.heartbeat_all();
        }
    });

    const bool ok = srv.listen(opt.host.c_str(), opt.port);

    running.store(false, std::memory_order_relaxed);
    heartbeat.join();

    if (!ok) {
        std::fprintf(stderr, "matchbook-server: failed to bind %s:%d\n",
                     opt.host.c_str(), opt.port);
        return 1;
    }
    return 0;
}
