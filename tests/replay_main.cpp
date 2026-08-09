// matchbook-replay — runs a recorded order stream through one engine and writes
// the canonical event log.
//
// This is the C++ half of the differential oracle. tools/reference_matcher.py
// produces the same log from the same input; tools/validate_fills.py diffs
// them. It is also the replay-determinism tool: running the same input twice
// through the same engine must produce byte-identical output, and running it
// through all four engines must produce four identical files.
//
// Input format (see tools/flowgen.py):
//   N,<client_id>,<participant>,<side>,<type>,<price|->,<qty>
//   C,<client_id>,<participant>
//   R,<orig_id>,<new_id>,<participant>,<price|->,<qty>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "matchbook/book.hpp"
#include "matchbook/engine.hpp"

namespace {

using namespace matchbook;

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream is(s);
    while (std::getline(is, cur, sep)) out.push_back(cur);
    return out;
}

bool parse_side(const std::string& s, Side& out) {
    if (s == "B" || s == "BUY")  { out = Side::Buy;  return true; }
    if (s == "S" || s == "SELL") { out = Side::Sell; return true; }
    return false;
}

bool parse_type(const std::string& s, OrdType& out) {
    if (s == "LIMIT")     { out = OrdType::Limit;    return true; }
    if (s == "MARKET")    { out = OrdType::Market;   return true; }
    if (s == "IOC")       { out = OrdType::IOC;      return true; }
    if (s == "FOK")       { out = OrdType::FOK;      return true; }
    if (s == "POST_ONLY") { out = OrdType::PostOnly; return true; }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    std::string input   = "-";
    std::string output  = "-";
    std::string final_book;
    EngineVersion version = EngineVersion::V0_Naive;

    BookConfig cfg;
    cfg.symbol           = "REPLAY";
    cfg.tick_size        = 1;
    cfg.reference_price  = 100'000;
    cfg.price_band_ticks = 10'000;
    cfg.max_orders       = 1u << 20;
    cfg.flat_levels      = 1u << 16;
    // Off during replay: paranoid mode would abort on a violation, and the
    // point of the replay tool is to produce a log that can be *diffed* to find
    // out what went wrong.
    cfg.invariant_mode   = InvariantMode::Off;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--input")            input = next();
        else if (a == "--output")      output = next();
        else if (a == "--final-book")  final_book = next();
        else if (a == "--tick")        cfg.tick_size = std::atoll(next());
        else if (a == "--reference")   cfg.reference_price = std::atoll(next());
        else if (a == "--band-ticks")  cfg.price_band_ticks = std::atoll(next());
        else if (a == "--engine") {
            if (!parse_engine_version(next(), version)) {
                std::fprintf(stderr, "unknown engine (use v0|v1|v2|v3)\n");
                return 2;
            }
        } else if (a == "--help" || a == "-h") {
            std::printf(
                "matchbook-replay --engine v0|v1|v2|v3 [--input FILE] [--output FILE]\n"
                "                 [--final-book FILE] [--tick N] [--reference N]\n"
                "\nRuns a recorded order stream and writes the canonical event log.\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument '%s'\n", a.c_str());
            return 2;
        }
    }

    Engine engine(version);
    engine.add_symbol(cfg);

    std::ifstream fin;
    std::istream* in = &std::cin;
    if (input != "-") {
        fin.open(input);
        if (!fin) { std::fprintf(stderr, "cannot open %s\n", input.c_str()); return 1; }
        in = &fin;
    }

    std::string out_text;
    out_text.reserve(1 << 20);

    EventSink sink(64);
    std::string line;
    std::size_t line_no = 0;
    std::size_t skipped = 0;

    while (std::getline(*in, line)) {
        ++line_no;
        if (line.empty() || line[0] == '#') continue;

        const std::vector<std::string> f = split(line, ',');
        if (f.empty()) continue;

        sink.clear();

        if (f[0] == "N" && f.size() == 7) {
            OrderRequest req;
            req.symbol      = cfg.symbol;
            req.client_id   = std::strtoull(f[1].c_str(), nullptr, 10);
            req.participant = static_cast<ParticipantId>(std::strtoul(f[2].c_str(), nullptr, 10));
            if (!parse_side(f[3], req.side) || !parse_type(f[4], req.type)) {
                ++skipped;
                continue;
            }
            req.price    = (f[5] == "-") ? kNoPrice : std::strtoll(f[5].c_str(), nullptr, 10);
            req.quantity = std::strtoll(f[6].c_str(), nullptr, 10);
            engine.submit(req, sink);

        } else if (f[0] == "C" && f.size() == 3) {
            CancelRequest req;
            req.symbol      = cfg.symbol;
            req.client_id   = std::strtoull(f[1].c_str(), nullptr, 10);
            req.participant = static_cast<ParticipantId>(std::strtoul(f[2].c_str(), nullptr, 10));
            engine.cancel(req, sink);

        } else if (f[0] == "R" && f.size() == 6) {
            ReplaceRequest req;
            req.original_client_id = std::strtoull(f[1].c_str(), nullptr, 10);
            req.new_client_id      = std::strtoull(f[2].c_str(), nullptr, 10);
            req.participant        = static_cast<ParticipantId>(std::strtoul(f[3].c_str(), nullptr, 10));
            req.new_price          = (f[4] == "-") ? kNoPrice : std::strtoll(f[4].c_str(), nullptr, 10);
            req.new_quantity       = std::strtoll(f[5].c_str(), nullptr, 10);
            engine.replace(cfg.symbol, req, sink);

        } else {
            ++skipped;
            continue;
        }

        out_text += sink.to_text();
    }

    std::ofstream fout;
    std::ostream* out = &std::cout;
    if (output != "-") {
        fout.open(output);
        if (!fout) { std::fprintf(stderr, "cannot write %s\n", output.c_str()); return 1; }
        out = &fout;
    }
    *out << out_text;
    out->flush();

    if (!final_book.empty()) {
        const IBook* book = engine.book(cfg.symbol);
        std::ofstream bf(final_book);
        if (!bf) { std::fprintf(stderr, "cannot write %s\n", final_book.c_str()); return 1; }
        // Same shape reference_matcher.py writes, so final states diff literally.
        const BookSnapshot snap = book->snapshot(1'000'000);
        for (const LevelView& l : snap.bids) {
            bf << "BID," << l.price << ',' << l.quantity << ',' << l.order_count << '\n';
        }
        for (const LevelView& l : snap.asks) {
            bf << "ASK," << l.price << ',' << l.quantity << ',' << l.order_count << '\n';
        }
    }

    if (skipped > 0) {
        std::fprintf(stderr, "matchbook-replay: skipped %zu malformed line(s) of %zu\n",
                     skipped, line_no);
    }
    return 0;
}
