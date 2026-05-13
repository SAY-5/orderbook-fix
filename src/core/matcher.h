// SPDX-License-Identifier: MIT
#pragma once

#include "events.h"
#include "order.h"
#include "order_book.h"
#include "price_level.h"
#include "types.h"

#include <memory>

namespace obfix {

// Matches an inbound order against the book according to `algo`. Any
// residual qty after matching is rested. Generated trades and acks are
// appended to `out`. Returns the same Order pointer (now owned by the
// book if it has leaves_qty > 0).
class Matcher {
public:
    explicit Matcher(MatchAlgo algo) : algo_(algo) {}

    // Set the algorithm at runtime (the env var is read once at start, the
    // setter exists so tests can flip between modes without rebuilding).
    void set_algo(MatchAlgo a) noexcept { algo_ = a; }
    MatchAlgo algo() const noexcept { return algo_; }

    // Match an aggressive order against `book`. Ownership of `incoming`
    // is taken; if residual remains it is rested into the book and the
    // raw pointer is kept inside the book's index.
    void submit(OrderBook& book, std::unique_ptr<Order> incoming, EventList& out);

private:
    // Walk the opposing side level by level and try to fill `aggr`.
    void match_aggressive(OrderBook& book, Order& aggr, EventList& out);

    // Allocate `available` qty across one price level using pro-rata.
    // Residual after rounding is filled FIFO across orders with leftover qty.
    // Returns the amount actually filled (<= available).
    Quantity fill_level_prorata(PriceLevel& level, Order& aggr, Price level_price,
                                Quantity available, EventList& out);

    // Strict FIFO walk of one price level.
    Quantity fill_level_fifo(PriceLevel& level, Order& aggr, Price level_price, Quantity available,
                             EventList& out);

    MatchAlgo algo_;
};

}  // namespace obfix
