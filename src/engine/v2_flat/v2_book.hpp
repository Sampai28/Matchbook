#pragma once

// V2 — cache-conscious price levels.
//
// The std::map ladder is replaced by a flat contiguous array indexed by
// ticks-from-base, with a two-level occupancy bitmap for O(1)-ish
// best-bid/best-ask. See the header comment in v2_book.cpp for when this is
// the wrong data structure entirely.

#include <memory>

#include "matchbook/book.hpp"

namespace matchbook::v2 {

std::unique_ptr<IBook> make(const BookConfig& cfg);

}  // namespace matchbook::v2
