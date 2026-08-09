#pragma once

// Shared infrastructure for V1, V2 and V3: a fixed-capacity order pool and the
// intrusive doubly-linked list that price levels are built from.
//
// V0 deliberately does not use either of these. The difference between V0 and
// V1 is exactly this file.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "matchbook/order.hpp"
#include "matchbook/types.hpp"

namespace matchbook::detail {

// A fixed-capacity pool of Order records with an intrusive free list.
//
// Two things a Python programmer should know about why this exists:
//
//   1. `new`/`malloc` for a small object under glibc costs on the order of
//      20-100ns once you include the free-list walk, the occasional arena lock,
//      and the cache miss on the chunk header. At a million orders a second
//      that is a meaningful fraction of the budget, and it is entirely avoidable
//      because the maximum number of live orders is known in advance.
//
//   2. `storage_` is sized once in the constructor and never resized. That is
//      load-bearing, not incidental: every resting order is referred to by raw
//      pointer from the price levels and the order index, and a std::vector
//      reallocation moves its elements, which would leave every one of those
//      pointers dangling. Growth is therefore not "unimplemented" — it is
//      forbidden. Exhaustion is reported as RejectReason::BookFull.
//
// The free list threads through the `next` pointer of the free records
// themselves, so it costs no extra memory. A free Order is not a valid order;
// its `next` is a free-list link, not a queue link.
class OrderPool {
public:
    explicit OrderPool(std::size_t capacity) : storage_(capacity) {
        // Link every slot into the free list, back to front, so that acquire()
        // hands out slot 0 first. Ascending allocation order gives the first
        // orders in a run contiguous addresses, which helps the prefetcher.
        for (std::size_t i = capacity; i-- > 0;) {
            Order& o    = storage_[i];
            o.pool_slot = static_cast<std::int32_t>(i);
            o.next      = free_head_;
            free_head_  = &o;
        }
    }

    OrderPool(const OrderPool&)            = delete;
    OrderPool& operator=(const OrderPool&) = delete;

    // Returns nullptr when exhausted. Callers must handle it; the engine turns
    // it into a BookFull rejection rather than aborting.
    [[nodiscard]] Order* acquire() noexcept {
        if (free_head_ == nullptr) return nullptr;
        Order* o   = free_head_;
        free_head_ = o->next;

        const std::int32_t slot = o->pool_slot;
        o->reset();          // reset() deliberately preserves pool_slot
        o->pool_slot = slot;
        ++in_use_;
        return o;
    }

    void release(Order* o) noexcept {
        if (o == nullptr) return;
        o->prev        = nullptr;
        o->level_index = -1;
        o->next        = free_head_;
        free_head_     = o;
        --in_use_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return storage_.size(); }
    [[nodiscard]] std::size_t in_use() const noexcept { return in_use_; }
    [[nodiscard]] bool exhausted() const noexcept { return free_head_ == nullptr; }

    // Stable across the pool's lifetime; used by V2/V3 to store slot indices
    // instead of pointers where an index is smaller.
    [[nodiscard]] Order* at(std::int32_t slot) noexcept { return &storage_[static_cast<std::size_t>(slot)]; }

private:
    std::vector<Order> storage_;
    Order*             free_head_ = nullptr;
    std::size_t        in_use_    = 0;
};

// An intrusive FIFO queue of Orders, forming one price level.
//
// "Intrusive" means the links live in the Order itself (Order::prev/next), so
// pushing costs no allocation and unlinking needs only the Order's address.
// Cancelling a resting order is therefore O(1) with no search: the order index
// hands back the pointer, and the order removes itself.
//
// total_qty is maintained incrementally so top-of-book depth is O(1). The
// invariant checker recomputes it by walking the list and compares — that
// redundancy is the point, since an incrementally-maintained aggregate is
// exactly the kind of thing that silently drifts.
struct IntrusiveList {
    Order*        head  = nullptr;
    Order*        tail  = nullptr;
    std::uint32_t count = 0;
    Qty           total_qty = 0;

    [[nodiscard]] bool empty() const noexcept { return head == nullptr; }
    [[nodiscard]] Order* front() const noexcept { return head; }

    void push_back(Order* o) noexcept {
        o->prev = tail;
        o->next = nullptr;
        if (tail != nullptr) {
            tail->next = o;
        } else {
            head = o;
        }
        tail = o;
        ++count;
        total_qty += o->remaining();
    }

    void unlink(Order* o) noexcept {
        if (o->prev != nullptr) {
            o->prev->next = o->next;
        } else {
            head = o->next;
        }
        if (o->next != nullptr) {
            o->next->prev = o->prev;
        } else {
            tail = o->prev;
        }
        o->prev = nullptr;
        o->next = nullptr;
        --count;
        total_qty -= o->remaining();
    }

    // Called when a resting order is partially filled: the order stays in the
    // queue but the level's aggregate must shrink.
    void reduce(Qty by) noexcept { total_qty -= by; }

    void clear() noexcept {
        head = tail = nullptr;
        count = 0;
        total_qty = 0;
    }

    // Recomputed from scratch for the invariant checker.
    [[nodiscard]] Qty walk_qty() const noexcept {
        Qty sum = 0;
        for (const Order* o = head; o != nullptr; o = o->next) sum += o->remaining();
        return sum;
    }

    [[nodiscard]] std::uint32_t walk_count() const noexcept {
        std::uint32_t n = 0;
        for (const Order* o = head; o != nullptr; o = o->next) ++n;
        return n;
    }
};

}  // namespace matchbook::detail
