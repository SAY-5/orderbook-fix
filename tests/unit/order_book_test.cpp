// SPDX-License-Identifier: MIT
#include "core/order_book.h"
#include "core/order.h"

#include <gtest/gtest.h>
#include <memory>

using namespace obfix;

namespace {
std::unique_ptr<Order> mk(OrderId id, Side s, Price p, Quantity q, const std::string& cl) {
    auto o = std::make_unique<Order>();
    o->id = id;
    o->side = s;
    o->price = p;
    o->orig_qty = q;
    o->leaves_qty = q;
    o->clord_id = ClOrdID{cl};
    return o;
}
}  // namespace

TEST(OrderBook, EmptyHasNoBest) {
    OrderBook b("SYM");
    EXPECT_FALSE(b.best_bid().has_value());
    EXPECT_FALSE(b.best_ask().has_value());
}

TEST(OrderBook, BestBidIsMaxPrice) {
    OrderBook b("SYM");
    b.rest(mk(1, Side::Buy, 10000, 10, "a"));
    b.rest(mk(2, Side::Buy, 10005, 10, "b"));
    b.rest(mk(3, Side::Buy, 9999, 10, "c"));
    ASSERT_TRUE(b.best_bid().has_value());
    EXPECT_EQ(*b.best_bid(), 10005);
}

TEST(OrderBook, BestAskIsMinPrice) {
    OrderBook b("SYM");
    b.rest(mk(1, Side::Sell, 10010, 10, "a"));
    b.rest(mk(2, Side::Sell, 10005, 10, "b"));
    b.rest(mk(3, Side::Sell, 10020, 10, "c"));
    ASSERT_TRUE(b.best_ask().has_value());
    EXPECT_EQ(*b.best_ask(), 10005);
}

TEST(OrderBook, FindByClOrdID) {
    OrderBook b("SYM");
    b.rest(mk(1, Side::Buy, 10000, 10, "abc"));
    EXPECT_NE(b.find(0, ClOrdID{"abc"}), nullptr);
    EXPECT_EQ(b.find(0, ClOrdID{"missing"}), nullptr);
}

TEST(OrderBook, CancelRemovesOrder) {
    OrderBook b("SYM");
    b.rest(mk(1, Side::Buy, 10000, 10, "abc"));
    auto r = b.cancel(0, ClOrdID{"abc"});
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(b.find(0, ClOrdID{"abc"}), nullptr);
    EXPECT_FALSE(b.best_bid().has_value());
}

TEST(OrderBook, CancelMissingFails) {
    OrderBook b("SYM");
    auto r = b.cancel(0, ClOrdID{"nope"});
    EXPECT_FALSE(r.ok);
}

TEST(OrderBook, RemoveDropsLevelWhenEmpty) {
    OrderBook b("SYM");
    auto o1 = mk(1, Side::Sell, 10000, 10, "a");
    Order* p1 = o1.get();
    b.rest(std::move(o1));
    b.remove(p1);
    EXPECT_FALSE(b.best_ask().has_value());
}

TEST(OrderBook, MultiSessionClOrdIDIsolation) {
    OrderBook b("SYM");
    auto a = mk(1, Side::Buy, 10000, 10, "shared");
    a->session = 1;
    auto c = mk(2, Side::Buy, 10000, 20, "shared");
    c->session = 2;
    b.rest(std::move(a));
    b.rest(std::move(c));
    EXPECT_NE(b.find(1, ClOrdID{"shared"}), nullptr);
    EXPECT_NE(b.find(2, ClOrdID{"shared"}), nullptr);
    EXPECT_NE(b.find(1, ClOrdID{"shared"}), b.find(2, ClOrdID{"shared"}));
}
