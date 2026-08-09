#pragma once

// V1 — allocation removal.
//
// Same algorithm as V0, same event stream, different memory behaviour:
// orders come from a pre-allocated pool and price levels are intrusive lists,
// so enqueueing an order allocates nothing. The price ladder is still a
// std::map, which is what V2 addresses.

#include <memory>

#include "matchbook/book.hpp"

namespace matchbook::v1 {

std::unique_ptr<IBook> make(const BookConfig& cfg);

}  // namespace matchbook::v1
