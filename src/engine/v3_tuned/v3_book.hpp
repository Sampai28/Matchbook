#pragma once

// V3 — hot-path tuning on top of V2's flat layout.
//
// Same data structures, same event stream. The changes are all about how many
// cache lines an operation touches and how often the branch predictor is wrong.

#include <memory>

#include "matchbook/book.hpp"

namespace matchbook::v3 {

std::unique_ptr<IBook> make(const BookConfig& cfg);

}  // namespace matchbook::v3
