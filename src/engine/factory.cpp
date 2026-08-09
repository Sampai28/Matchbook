// Runtime selection of the engine implementation.
//
// All four versions are compiled into every binary. The differential oracle and
// the benchmark harness both need to drive several versions in one process, so
// choosing at link time would not work.

#include "matchbook/book.hpp"

#include <string_view>

#include "engine/v0_naive/v0_book.hpp"
#include "engine/v1_pool/v1_book.hpp"
#include "engine/v2_flat/v2_book.hpp"
#include "engine/v3_tuned/v3_book.hpp"

namespace matchbook {

bool parse_engine_version(std::string_view name, EngineVersion& out) noexcept {
    if (name == "v0" || name == "V0" || name == "naive") { out = EngineVersion::V0_Naive; return true; }
    if (name == "v1" || name == "V1" || name == "pool")  { out = EngineVersion::V1_Pool;  return true; }
    if (name == "v2" || name == "V2" || name == "flat")  { out = EngineVersion::V2_Flat;  return true; }
    if (name == "v3" || name == "V3" || name == "tuned") { out = EngineVersion::V3_Tuned; return true; }
    return false;
}

std::unique_ptr<IBook> make_book(EngineVersion version, const BookConfig& cfg) {
    switch (version) {
        case EngineVersion::V0_Naive: return v0::make(cfg);
        case EngineVersion::V1_Pool:  return v1::make(cfg);
        case EngineVersion::V2_Flat:  return v2::make(cfg);
        case EngineVersion::V3_Tuned: return v3::make(cfg);
    }
    // Unreachable for a valid enum value. Falling back to the reference
    // implementation is safer than returning nullptr, which would crash at the
    // first call rather than merely being slow.
    return v0::make(cfg);
}

}  // namespace matchbook
