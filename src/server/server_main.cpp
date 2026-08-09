// matchbook-server — HTTP front end over the engine.
//
// Single-threaded on purpose. The engine has no locking anywhere, so cpp-httplib
// is configured with one worker thread; a second would corrupt the book. This is
// the single most important line in the file to not "fix" later.
//
//   POST   /order                     submit a new order
//   DELETE /order/{client_id}         cancel, ?symbol=&participant=
//   GET    /book/{symbol}?depth=N     depth-of-book ladder
//   GET    /stats                     counters, rejections, latency percentiles
//   GET    /healthz                   liveness
//   GET    /                          static ladder viewer from web/

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

#include <httplib.h>

#include "matchbook/engine.hpp"
#include "matchbook/metrics.hpp"
#include "matchbook/types.hpp"
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
    std::uint64_t    http_requests = 0;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
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
    const auto t0 = std::chrono::steady_clock::now();
    const SubmitResult r = st.engine->submit(o, sink);
    const auto t1 = std::chrono::steady_clock::now();
    st.submit_latency.record(
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));

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
    const auto t0 = std::chrono::steady_clock::now();
    const CancelResult r = st.engine->cancel(c, sink);
    const auto t1 = std::chrono::steady_clock::now();
    st.cancel_latency.record(
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));

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

void handle_stats(ServerState& st, const httplib::Request&, httplib::Response& res) {
    const Stats s = st.engine->aggregate_stats();

    json::Writer w;
    w.begin_object();
    w.field("engine", to_string(st.engine->version()));
    w.field("uptimeSeconds",
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - st.started).count()));
    w.field("httpRequests", st.http_requests);

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

    // ONE worker thread. The engine has no locks; concurrent requests would
    // corrupt the book. Do not raise this without making the engines thread-safe.
    srv.new_task_queue = [] { return new httplib::ThreadPool(1); };

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
    std::fflush(stdout);

    if (!srv.listen(opt.host.c_str(), opt.port)) {
        std::fprintf(stderr, "matchbook-server: failed to bind %s:%d\n",
                     opt.host.c_str(), opt.port);
        return 1;
    }
    return 0;
}
