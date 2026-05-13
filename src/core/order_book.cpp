// SPDX-License-Identifier: MIT
#include "order_book.h"

namespace obfix {

void OrderBook::rest(std::unique_ptr<Order> order) {
    Order* raw = order.get();
    if (raw->side == Side::Buy) {
        bids_[raw->price].push_back(raw);
    } else {
        asks_[raw->price].push_back(raw);
    }
    IndexKey k{raw->session, raw->clord_id.value};
    index_.emplace(std::move(k), std::move(order));
}

Order* OrderBook::find(SessionId s, const ClOrdID& clord) noexcept {
    IndexKey k{s, clord.value};
    auto it = index_.find(k);
    return it == index_.end() ? nullptr : it->second.get();
}

const Order* OrderBook::find(SessionId s, const ClOrdID& clord) const noexcept {
    IndexKey k{s, clord.value};
    auto it = index_.find(k);
    return it == index_.end() ? nullptr : it->second.get();
}

CancelResult OrderBook::cancel(SessionId s, const ClOrdID& clord) {
    IndexKey k{s, clord.value};
    auto it = index_.find(k);
    if (it == index_.end()) return {false, "unknown order"};
    Order* o = it->second.get();
    if (o->side == Side::Buy) {
        auto lit = bids_.find(o->price);
        if (lit != bids_.end()) lit->second.unlink(o);
    } else {
        auto lit = asks_.find(o->price);
        if (lit != asks_.end()) lit->second.unlink(o);
    }
    index_.erase(it);
    return {true, ""};
}

void OrderBook::remove(Order* o) {
    if (o->side == Side::Buy) {
        auto lit = bids_.find(o->price);
        if (lit != bids_.end()) lit->second.unlink(o);
    } else {
        auto lit = asks_.find(o->price);
        if (lit != asks_.end()) lit->second.unlink(o);
    }
    index_.erase(IndexKey{o->session, o->clord_id.value});
}

std::optional<Price> OrderBook::best_bid() const {
    for (const auto& [p, lvl] : bids_) {
        if (!lvl.empty()) return p;
    }
    return std::nullopt;
}

std::optional<Price> OrderBook::best_ask() const {
    for (const auto& [p, lvl] : asks_) {
        if (!lvl.empty()) return p;
    }
    return std::nullopt;
}

}  // namespace obfix
