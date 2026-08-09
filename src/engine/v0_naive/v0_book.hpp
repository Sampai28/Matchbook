#pragma once

// V0 — the naive baseline.
//
// Written for obvious correctness, not speed. It is the reference every other
// version is diffed against, so it stays deliberately boring: standard
// containers, no pooling, no bit tricks, nothing clever enough to be wrong in a
// subtle way.

#include <memory>

#include "matchbook/book.hpp"

namespace matchbook::v0 {

std::unique_ptr<IBook> make(const BookConfig& cfg);

}  // namespace matchbook::v0
