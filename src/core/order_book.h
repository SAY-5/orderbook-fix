// SPDX-License-Identifier: MIT
#pragma once

#include "events.h"
#include "order.h"
#include "price_level.h"
#include "types.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace obfix {

struct CancelResult {
    bool ok;
    std::string reason;
};

class OrderBook {
public:
    explicit OrderBook(std::string symbol) : symbol_(std::move(symbol)) {}

    // Insert without matching (used by tests and the replace path).
    void rest(std::unique_ptr<Order> order);

    // Look up by ClOrdID (a session-scoped key from FIX tag 11). The book
    // is per-symbol but the index is keyed by (session, ClOrdID) since the
    // FIX spec allows reuse across sessions.
    Order* find(SessionId s, const ClOrdID& clord) noexcept;
    const Order* find(SessionId s, const ClOrdID& clord) const noexcept;

    // Cancel an existing order. Returns ok=false if it does not exist.
    CancelResult cancel(SessionId s, const ClOrdID& clord);

    // Iterate the bid book best-first (highest price). Stops when fn returns false.
    template <class Fn>
    void walk_bids(Fn&& fn) {
        for (auto it = bids_.begin(); it != bids_.end();) {
            auto& level = it->second;
            if (level.empty()) {
                it = bids_.erase(it);
                continue;
            }
            if (!fn(it->first, level)) return;
            if (level.empty())
                it = bids_.erase(it);
            else
                ++it;
        }
    }

    template <class Fn>
    void walk_asks(Fn&& fn) {
        for (auto it = asks_.begin(); it != asks_.end();) {
            auto& level = it->second;
            if (level.empty()) {
                it = asks_.erase(it);
                continue;
            }
            if (!fn(it->first, level)) return;
            if (level.empty())
                it = asks_.erase(it);
            else
                ++it;
        }
    }

    // For matcher use: fetch the level at a specific price.
    PriceLevel* bid_level(Price p) {
        auto it = bids_.find(p);
        return it == bids_.end() ? nullptr : &it->second;
    }
    PriceLevel* ask_level(Price p) {
        auto it = asks_.find(p);
        return it == asks_.end() ? nullptr : &it->second;
    }

    // Best prices (nullopt if side empty).
    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    const std::string& symbol() const noexcept { return symbol_; }

    // Erase an order entirely (the matcher uses this when leaves_qty hits 0).
    void remove(Order* o);

private:
    using BidMap = std::map<Price, PriceLevel, std::greater<Price>>;
    using AskMap = std::map<Price, PriceLevel, std::less<Price>>;

    struct IndexKey {
        SessionId s;
        std::string c;
        bool operator==(const IndexKey& o) const { return s == o.s && c == o.c; }
    };
    struct IndexHash {
        std::size_t operator()(const IndexKey& k) const noexcept {
            std::size_t h = std::hash<std::string>{}(k.c);
            h ^= std::hash<SessionId>{}(k.s) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            return h;
        }
    };

    std::string symbol_;
    BidMap bids_;
    AskMap asks_;
    std::unordered_map<IndexKey, std::unique_ptr<Order>, IndexHash> index_;
};

}  // namespace obfix
