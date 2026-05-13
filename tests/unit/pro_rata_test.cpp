// SPDX-License-Identifier: MIT
#include "core/matcher.h"
#include "core/order.h"
#include "core/order_book.h"

#include <gtest/gtest.h>

#include <memory>

using namespace obfix;

namespace {

std::unique_ptr<Order> make_order(OrderId id, Side s, Price p, Quantity q,
                                  const std::string& clord = "") {
    auto o = std::make_unique<Order>();
    o->id = id;
    o->side = s;
    o->price = p;
    o->orig_qty = q;
    o->leaves_qty = q;
    o->clord_id = ClOrdID{clord.empty() ? std::to_string(id) : clord};
    return o;
}

// Sum of fill qty per resting order from the trade log. Resting orders
// are identified by their id; the aggressor's id is excluded.
std::map<OrderId, Quantity> fills_by_resting(const EventList& evs) {
    std::map<OrderId, Quantity> m;
    for (const auto& e : evs) {
        if (std::holds_alternative<Trade>(e)) {
            const auto& t = std::get<Trade>(e);
            m[t.resting_id] += t.qty;
        }
    }
    return m;
}

}  // namespace

TEST(ProRata, EqualSizeOrdersSplitEvenly) {
    OrderBook book("SYM");
    book.rest(make_order(1, Side::Sell, 10000, 100));
    book.rest(make_order(2, Side::Sell, 10000, 100));
    book.rest(make_order(3, Side::Sell, 10000, 100));

    Matcher m(MatchAlgo::ProRata);
    EventList out;
    m.submit(book, make_order(99, Side::Buy, 10000, 300), out);

    auto f = fills_by_resting(out);
    EXPECT_EQ(f[1], 100u);
    EXPECT_EQ(f[2], 100u);
    EXPECT_EQ(f[3], 100u);
}

TEST(ProRata, ThreeHundredThreeHundredFourHundredSpecExample) {
    // README's worked example: aggressor 1000 at $100 vs three resting at $100
    // sized 300/300/400 -> fills 300/300/400.
    OrderBook book("SYM");
    book.rest(make_order(1, Side::Sell, 10000, 300));
    book.rest(make_order(2, Side::Sell, 10000, 300));
    book.rest(make_order(3, Side::Sell, 10000, 400));

    Matcher m(MatchAlgo::ProRata);
    EventList out;
    m.submit(book, make_order(99, Side::Buy, 10000, 1000), out);

    auto f = fills_by_resting(out);
    EXPECT_EQ(f[1], 300u);
    EXPECT_EQ(f[2], 300u);
    EXPECT_EQ(f[3], 400u);
}

TEST(ProRata, ProportionalAllocationWithRounding) {
    // total = 600, aggressor wants 500.
    // 100 -> floor(500 * 100 / 600) = 83
    // 200 -> floor(500 * 200 / 600) = 166
    // 300 -> floor(500 * 300 / 600) = 250
    // sum = 499; residual 1 goes FIFO to order id 1.
    OrderBook book("SYM");
    book.rest(make_order(1, Side::Sell, 10000, 100));
    book.rest(make_order(2, Side::Sell, 10000, 200));
    book.rest(make_order(3, Side::Sell, 10000, 300));

    Matcher m(MatchAlgo::ProRata);
    EventList out;
    m.submit(book, make_order(99, Side::Buy, 10000, 500), out);

    auto f = fills_by_resting(out);
    EXPECT_EQ(f[1], 84u);  // 83 + 1 residual
    EXPECT_EQ(f[2], 166u);
    EXPECT_EQ(f[3], 250u);
    EXPECT_EQ(f[1] + f[2] + f[3], 500u);
}

TEST(ProRata, AggressorLargerThanLevelFillsEverything) {
    OrderBook book("SYM");
    book.rest(make_order(1, Side::Sell, 10000, 100));
    book.rest(make_order(2, Side::Sell, 10000, 200));

    Matcher m(MatchAlgo::ProRata);
    EventList out;
    m.submit(book, make_order(99, Side::Buy, 10000, 1000), out);

    auto f = fills_by_resting(out);
    EXPECT_EQ(f[1], 100u);
    EXPECT_EQ(f[2], 200u);
    // residual 700 is rested as a bid.
    EXPECT_TRUE(book.best_bid().has_value());
    EXPECT_EQ(*book.best_bid(), 10000);
}

TEST(ProRata, SingleOrderAtLevelGetsEverything) {
    OrderBook book("SYM");
    book.rest(make_order(1, Side::Sell, 10000, 500));

    Matcher m(MatchAlgo::ProRata);
    EventList out;
    m.submit(book, make_order(99, Side::Buy, 10000, 300), out);

    auto f = fills_by_resting(out);
    EXPECT_EQ(f[1], 300u);
}

TEST(ProRata, ResidualUsesFifoTiebreaker) {
    // 5 orders of size 7 at same price; aggressor 11.
    // 11 * 7 / 35 = 2 each, sum 10, residual 1 -> goes to first (id 1).
    OrderBook book("SYM");
    for (int i = 1; i <= 5; ++i) book.rest(make_order(i, Side::Sell, 10000, 7));

    Matcher m(MatchAlgo::ProRata);
    EventList out;
    m.submit(book, make_order(99, Side::Buy, 10000, 11), out);

    auto f = fills_by_resting(out);
    EXPECT_EQ(f[1], 3u);
    EXPECT_EQ(f[2], 2u);
    EXPECT_EQ(f[3], 2u);
    EXPECT_EQ(f[4], 2u);
    EXPECT_EQ(f[5], 2u);
}

TEST(ProRata, AcrossMultiplePriceLevels) {
    OrderBook book("SYM");
    book.rest(make_order(1, Side::Sell, 10000, 100));  // best ask
    book.rest(make_order(2, Side::Sell, 10001, 200));
    book.rest(make_order(3, Side::Sell, 10001, 200));

    Matcher m(MatchAlgo::ProRata);
    EventList out;
    m.submit(book, make_order(99, Side::Buy, 10001, 300), out);

    auto f = fills_by_resting(out);
    EXPECT_EQ(f[1], 100u);  // fully fills the best ask
    EXPECT_EQ(f[2] + f[3], 200u);
    EXPECT_EQ(f[2], 100u);
    EXPECT_EQ(f[3], 100u);
}

TEST(ProRata, FullyFilledOrdersAreRemovedFromBook) {
    OrderBook book("SYM");
    book.rest(make_order(1, Side::Sell, 10000, 100));
    book.rest(make_order(2, Side::Sell, 10000, 100));

    Matcher m(MatchAlgo::ProRata);
    EventList out;
    m.submit(book, make_order(99, Side::Buy, 10000, 200), out);

    EXPECT_FALSE(book.best_ask().has_value());
}
