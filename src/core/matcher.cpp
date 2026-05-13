// SPDX-License-Identifier: MIT
#include "matcher.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace obfix {

namespace {

inline bool crosses(Side aggr_side, Price aggr_price, Price level_price) {
    // Limit-only book: a buy crosses an ask priced <= the buy's limit,
    // a sell crosses a bid priced >= the sell's limit.
    return aggr_side == Side::Buy ? level_price <= aggr_price : level_price >= aggr_price;
}

inline void emit_trade(EventList& out, Order& aggr, Order& resting, Price px, Quantity q) {
    Trade t{};
    t.aggressor_id = aggr.id;
    t.resting_id = resting.id;
    t.aggressor_clord = aggr.clord_id;
    t.resting_clord = resting.clord_id;
    t.price = px;
    t.qty = q;
    t.aggressor_side = aggr.side;
    out.emplace_back(t);
}

}  // namespace

Quantity Matcher::fill_level_prorata(PriceLevel& level, Order& aggr, Price level_price,
                                     Quantity available, EventList& out) {
    // Snapshot resting orders + leaves_qty so the proportional pass uses a
    // stable denominator. Without this, an order partially filled earlier
    // in the same pass would shrink the denominator and the next order
    // would absorb a disproportionate share.
    struct Slot {
        Order* o;
        Quantity orig_leaves;
        Quantity alloc;
    };
    std::vector<Slot> slots;
    slots.reserve(level.count());
    Quantity total = 0;
    for (Order* o = level.head(); o; o = o->next) {
        slots.push_back({o, o->leaves_qty, 0});
        total += o->leaves_qty;
    }
    if (total == 0 || available == 0) return 0;

    Quantity to_fill = std::min(available, total);

    // Step 1: floor(to_fill * size_i / total) for each resting order.
    Quantity allocated = 0;
    for (auto& s : slots) {
        Quantity num = static_cast<Quantity>(to_fill) * s.orig_leaves;
        Quantity alloc = num / total;
        s.alloc = alloc;
        allocated += alloc;
    }

    // Step 2: distribute the rounding residual FIFO across orders that
    // still have remaining capacity. Slots were built head-to-tail so this
    // preserves price-time priority for the tiebreaker.
    Quantity residual = to_fill - allocated;
    for (auto& s : slots) {
        if (residual == 0) break;
        Quantity remaining_cap = s.orig_leaves - s.alloc;
        Quantity take = std::min(remaining_cap, residual);
        s.alloc += take;
        residual -= take;
    }

    // Step 3: emit trades and decrement leaves_qty. The caller (submit)
    // sweeps the book's index after matching to drop any order at
    // leaves_qty == 0.
    Quantity filled = 0;
    std::int64_t level_delta = 0;
    for (auto& s : slots) {
        if (s.alloc == 0) continue;
        emit_trade(out, aggr, *s.o, level_price, s.alloc);
        s.o->leaves_qty -= s.alloc;
        level_delta -= static_cast<std::int64_t>(s.alloc);
        filled += s.alloc;
    }
    level.note_qty_delta(level_delta);
    aggr.leaves_qty -= filled;
    return filled;
}

Quantity Matcher::fill_level_fifo(PriceLevel& level, Order& aggr, Price level_price,
                                  Quantity available, EventList& out) {
    Quantity filled = 0;
    std::int64_t level_delta = 0;
    for (Order* o = level.head(); o && filled < available; o = o->next) {
        Quantity q = std::min(available - filled, o->leaves_qty);
        if (q == 0) continue;
        emit_trade(out, aggr, *o, level_price, q);
        o->leaves_qty -= q;
        level_delta -= static_cast<std::int64_t>(q);
        filled += q;
    }
    level.note_qty_delta(level_delta);
    aggr.leaves_qty -= filled;
    return filled;
}

void Matcher::match_aggressive(OrderBook& book, Order& aggr, EventList& out) {
    auto on_level = [&](Price level_price, PriceLevel& level) {
        if (aggr.leaves_qty == 0) return false;
        if (!crosses(aggr.side, aggr.price, level_price)) return false;
        if (algo_ == MatchAlgo::ProRata) {
            fill_level_prorata(level, aggr, level_price, aggr.leaves_qty, out);
        } else {
            fill_level_fifo(level, aggr, level_price, aggr.leaves_qty, out);
        }
        return aggr.leaves_qty > 0;
    };
    if (aggr.side == Side::Buy)
        book.walk_asks(on_level);
    else
        book.walk_bids(on_level);
}

void Matcher::submit(OrderBook& book, std::unique_ptr<Order> incoming, EventList& out) {
    Order* raw = incoming.get();

    // Ack first so a client always observes New before any partial fill.
    Ack a{};
    a.id = raw->id;
    a.clord_id = raw->clord_id;
    out.emplace_back(a);

    // Match against the opposite side.
    match_aggressive(book, *raw, out);

    // Sweep fully-filled resting orders out of the book index.
    auto sweep = [&](Price, PriceLevel& level) {
        Order* o = level.head();
        while (o) {
            Order* next = o->next;
            if (o->leaves_qty == 0) book.remove(o);
            o = next;
        }
        return true;
    };
    if (raw->side == Side::Buy)
        book.walk_asks(sweep);
    else
        book.walk_bids(sweep);

    // Rest residual if any.
    if (raw->leaves_qty > 0) {
        book.rest(std::move(incoming));
    }
}

}  // namespace obfix
