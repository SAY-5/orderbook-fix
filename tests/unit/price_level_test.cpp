// SPDX-License-Identifier: MIT
#include "core/price_level.h"
#include "core/order.h"

#include <gtest/gtest.h>

using namespace obfix;

TEST(PriceLevel, EmptyInvariants) {
    PriceLevel l;
    EXPECT_TRUE(l.empty());
    EXPECT_EQ(l.count(), 0u);
    EXPECT_EQ(l.total_qty(), 0u);
    EXPECT_EQ(l.head(), nullptr);
}

TEST(PriceLevel, PushBackMaintainsOrder) {
    PriceLevel l;
    Order a{}, b{}, c{};
    a.leaves_qty = 1;
    b.leaves_qty = 2;
    c.leaves_qty = 3;
    l.push_back(&a);
    l.push_back(&b);
    l.push_back(&c);
    EXPECT_EQ(l.count(), 3u);
    EXPECT_EQ(l.total_qty(), 6u);
    EXPECT_EQ(l.head(), &a);
    EXPECT_EQ(l.head()->next, &b);
    EXPECT_EQ(l.head()->next->next, &c);
}

TEST(PriceLevel, UnlinkRemovesNodeAndAdjustsTotal) {
    PriceLevel l;
    Order a{}, b{}, c{};
    a.leaves_qty = 1;
    b.leaves_qty = 2;
    c.leaves_qty = 3;
    l.push_back(&a);
    l.push_back(&b);
    l.push_back(&c);
    l.unlink(&b);
    EXPECT_EQ(l.count(), 2u);
    EXPECT_EQ(l.total_qty(), 4u);
    EXPECT_EQ(l.head(), &a);
    EXPECT_EQ(l.head()->next, &c);
}

TEST(PriceLevel, UnlinkHead) {
    PriceLevel l;
    Order a{}, b{};
    a.leaves_qty = 1;
    b.leaves_qty = 2;
    l.push_back(&a);
    l.push_back(&b);
    l.unlink(&a);
    EXPECT_EQ(l.head(), &b);
    EXPECT_EQ(b.prev, nullptr);
}

TEST(PriceLevel, UnlinkTail) {
    PriceLevel l;
    Order a{}, b{};
    a.leaves_qty = 1;
    b.leaves_qty = 2;
    l.push_back(&a);
    l.push_back(&b);
    l.unlink(&b);
    EXPECT_EQ(l.head(), &a);
    EXPECT_EQ(a.next, nullptr);
}

TEST(PriceLevel, QtyDeltaAccumulates) {
    PriceLevel l;
    Order a{};
    a.leaves_qty = 100;
    l.push_back(&a);
    l.note_qty_delta(-30);
    EXPECT_EQ(l.total_qty(), 70u);
    l.note_qty_delta(-70);
    EXPECT_EQ(l.total_qty(), 0u);
}
