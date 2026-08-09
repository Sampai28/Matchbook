// Fuzz target for the validation layer and the engine's inbound path.
//
// Builds two ways:
//
//   * With libFuzzer (clang, -DMATCHBOOK_LIBFUZZER=ON): the LLVMFuzzerTestOneInput
//     entry point below is used and coverage-guided mutation drives it.
//   * Without it (the default, g++ 13): a standalone main() runs a seeded
//     pseudo-random corpus. Far weaker than coverage-guided fuzzing, but it
//     needs no clang and runs in CI, which is the difference between a fuzz
//     target that exists and one that is actually exercised.
//
// The property under test is narrow and absolute: no byte sequence, however
// malformed, may cause a crash, an assertion, an abort, or an infinite loop.
// Rejection is always an acceptable outcome. Acceptance of nonsense is not
// tested here -- that is the differential oracle's job.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "matchbook/book.hpp"
#include "matchbook/engine.hpp"
#include "validation/validator.hpp"

namespace {

using namespace matchbook;

BookConfig fuzz_config() {
    BookConfig cfg;
    cfg.symbol           = "FUZZ";
    cfg.tick_size        = 1;
    cfg.reference_price  = 100'000;
    cfg.price_band_ticks = 10'000;
    cfg.max_orders       = 4096;
    cfg.flat_levels      = 4096;
    // Off, not Paranoid: paranoid mode aborts by design, and a fuzzer would
    // report that deliberate abort as a crash. Invariants are exercised by the
    // unit tests instead.
    cfg.invariant_mode   = InvariantMode::Off;
    return cfg;
}

// Interprets arbitrary bytes as a sequence of operations. Every field is read
// with memcpy from a bounded cursor, so a truncated or oversized input yields a
// short operation list rather than an out-of-bounds read.
struct Cursor {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t pos = 0;

    template <typename T>
    bool take(T& out) {
        if (pos + sizeof(T) > size) return false;
        std::memcpy(&out, data + pos, sizeof(T));
        pos += sizeof(T);
        return true;
    }
};

void drive(const std::uint8_t* data, std::size_t size) {
    Cursor cur{data, size};

    // The engine version is chosen from the input too, so the fuzzer explores
    // all four implementations rather than only the default.
    std::uint8_t vsel = 0;
    if (!cur.take(vsel)) return;
    const auto version = static_cast<EngineVersion>(vsel % 4);

    auto book = make_book(version, fuzz_config());
    Validator validator{fuzz_config()};
    EventSink sink(32);

    int operations = 0;
    // Bounded so a pathological input cannot produce an unbounded run that
    // looks like a hang.
    while (operations++ < 2048) {
        std::uint8_t op = 0;
        if (!cur.take(op)) break;

        std::uint64_t cid = 0;
        std::uint32_t part = 0;
        std::int64_t  price = 0;
        std::int64_t  qty = 0;
        std::uint8_t  flags = 0;

        if (!cur.take(cid))   break;
        if (!cur.take(part))  break;
        if (!cur.take(price)) break;
        if (!cur.take(qty))   break;
        if (!cur.take(flags)) break;

        sink.clear();

        switch (op % 3) {
            case 0: {
                NewOrder o;
                o.client_id   = cid;
                o.participant = part;
                o.side        = (flags & 1) ? Side::Sell : Side::Buy;
                o.type        = static_cast<OrdType>((flags >> 1) % 5);
                o.price       = (o.type == OrdType::Market) ? kNoPrice : price;
                o.quantity    = qty;

                // The gate runs first, exactly as the engine does it, so the
                // engine only ever sees inputs the validator approved.
                if (validator.validate_new(o) == RejectReason::None) {
                    book->submit(o, sink);
                }
                break;
            }
            case 1: {
                book->cancel(cid, part, sink);
                break;
            }
            case 2: {
                ReplaceRequest rq;
                rq.original_client_id = cid;
                rq.new_client_id      = cid ^ 0x9E3779B97F4A7C15ULL;
                rq.participant        = part;
                rq.new_price          = (flags & 0x80) ? kNoPrice : price;
                rq.new_quantity       = qty;
                if (validator.validate_replace(rq) == RejectReason::None) {
                    book->replace(rq, sink);
                }
                break;
            }
            default:
                break;
        }
    }

    // A final structural check. Nothing the validator let through should have
    // been able to corrupt the book.
    std::string detail;
    if (!book->check_invariants(detail)) {
        std::fprintf(stderr, "FUZZ: invariant violation after valid input:\n%s\n%s\n",
                     detail.c_str(), book->debug_dump().c_str());
        std::abort();
    }
}

}  // namespace

#if defined(MATCHBOOK_LIBFUZZER)

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    drive(data, size);
    return 0;
}

#else

// Standalone fallback: a seeded corpus, so a failure is reproducible from the
// seed alone rather than requiring the crashing input to be preserved.
int main(int argc, char** argv) {
    std::uint64_t seed = 0xDEADBEEF;
    int iterations = 2000;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            iterations = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("matchbook-fuzz [--seed N] [--iterations N]\n"
                        "Standalone corpus mode. For coverage-guided fuzzing, build\n"
                        "with clang and -DMATCHBOOK_LIBFUZZER=ON.\n");
            return 0;
        }
    }

    std::mt19937_64 rng(seed);
    std::vector<std::uint8_t> buf;

    std::printf("matchbook-fuzz: %d iterations, seed %llu\n",
                iterations, static_cast<unsigned long long>(seed));

    for (int i = 0; i < iterations; ++i) {
        const std::size_t n = 1 + (rng() % 4096);
        buf.resize(n);
        for (std::size_t j = 0; j < n; ++j) {
            buf[j] = static_cast<std::uint8_t>(rng() & 0xFF);
        }
        drive(buf.data(), buf.size());

        if ((i + 1) % 250 == 0) {
            std::printf("  %d/%d\n", i + 1, iterations);
            std::fflush(stdout);
        }
    }

    std::printf("matchbook-fuzz: completed without a crash or invariant violation\n");
    return 0;
}

#endif
