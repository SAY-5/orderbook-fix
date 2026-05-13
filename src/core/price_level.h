// SPDX-License-Identifier: MIT
#pragma once

#include "order.h"
#include "types.h"
#include <cstddef>

namespace obfix {

// Each price level holds a FIFO queue of resting orders (head = oldest).
// The total aggregate qty is maintained incrementally so the pro-rata
// allocator does not need to walk the list to compute the denominator.
class PriceLevel {
public:
    void push_back(Order* o) noexcept {
        o->prev = tail_;
        o->next = nullptr;
        if (tail_)
            tail_->next = o;
        else
            head_ = o;
        tail_ = o;
        ++count_;
        total_qty_ += o->leaves_qty;
    }

    void unlink(Order* o) noexcept {
        if (o->prev)
            o->prev->next = o->next;
        else
            head_ = o->next;
        if (o->next)
            o->next->prev = o->prev;
        else
            tail_ = o->prev;
        o->prev = o->next = nullptr;
        --count_;
        total_qty_ -= o->leaves_qty;
    }

    // Adjust the cached total when an order's leaves_qty changes by `delta`
    // (negative if it shrank). Callers must keep this in sync.
    void note_qty_delta(std::int64_t delta) noexcept {
        // total_qty_ is unsigned; callers guarantee the result stays >= 0.
        total_qty_ = static_cast<Quantity>(static_cast<std::int64_t>(total_qty_) + delta);
    }

    Order* head() noexcept { return head_; }
    const Order* head() const noexcept { return head_; }
    Quantity total_qty() const noexcept { return total_qty_; }
    std::size_t count() const noexcept { return count_; }
    bool empty() const noexcept { return head_ == nullptr; }

private:
    Order* head_{nullptr};
    Order* tail_{nullptr};
    std::size_t count_{0};
    Quantity total_qty_{0};
};

}  // namespace obfix
