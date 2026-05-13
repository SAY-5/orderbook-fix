// SPDX-License-Identifier: MIT
#include "core/matcher.h"
#include "core/order.h"
#include "core/order_book.h"

#include <gtest/gtest.h>
#include <memory>

using namespace obfix;

namespace {
std::unique_ptr<Order> mk(OrderId id, Side s, Price p, Quantity q) {
    auto o = std::make_unique<Order>();
    o->id = id;
    o->side = s;
    o->price = p;
    o->orig_qty = q;
    o->leaves_qty = q;
    o->clord_id = ClOrdID{std::to_string(id)};
    return o;
}
Quantity fills_for(const EventList& evs, OrderId id) {
    Quantity q = 0;
    for (const auto& e : evs) {
        if (std::holds_alternative<Trade>(e)) {
            const auto& t = std::get<Trade>(e);
            if (t.resting_id == id) q += t.qty;
        }
    }
    return q;
}
}  // namespace

TEST(Matcher, FifoStrictPriceTime) {
    OrderBook book("SYM");
    book.rest(mk(1, Side::Sell, 10000, 100));
    book.rest(mk(2, Side::Sell, 10000, 100));
    book.rest(mk(3, Side::Sell, 10000, 100));

    Matcher m(MatchAlgo::Fifo);
    EventList out;
    m.submit(book, mk(99, Side::Buy, 10000, 150), out);

    EXPECT_EQ(fills_for(out, 1), 100u);
    EXPECT_EQ(fills_for(out, 2), 50u);
    EXPECT_EQ(fills_for(out, 3), 0u);
}

TEST(Matcher, ModeSwitchHotSwap) {
    OrderBook book("SYM");
    book.rest(mk(1, Side::Sell, 10000, 300));
    book.rest(mk(2, Side::Sell, 10000, 300));
    book.rest(mk(3, Side::Sell, 10000, 400));

    Matcher m(MatchAlgo::Fifo);
    EXPECT_EQ(m.algo(), MatchAlgo::Fifo);
    EventList out;
    m.submit(book, mk(99, Side::Buy, 10000, 500), out);
    EXPECT_EQ(fills_for(out, 1), 300u);
    EXPECT_EQ(fills_for(out, 2), 200u);
    EXPECT_EQ(fills_for(out, 3), 0u);

    out.clear();
    m.set_algo(MatchAlgo::ProRata);
    EXPECT_EQ(m.algo(), MatchAlgo::ProRata);
    // After the FIFO trade, level has order 2 (100 leaves) and 3 (400).
    // Aggressor 250 prorata: total=500, 100/500*250=50, 400/500*250=200.
    m.submit(book, mk(98, Side::Buy, 10000, 250), out);
    EXPECT_EQ(fills_for(out, 2), 50u);
    EXPECT_EQ(fills_for(out, 3), 200u);
}

TEST(Matcher, NoCrossDoesNothing) {
    OrderBook book("SYM");
    book.rest(mk(1, Side::Sell, 10100, 100));
    Matcher m(MatchAlgo::ProRata);
    EventList out;
    m.submit(book, mk(99, Side::Buy, 10000, 100), out);
    // Only the Ack event.
    int trades = 0;
    for (const auto& e : out)
        if (std::holds_alternative<Trade>(e)) ++trades;
    EXPECT_EQ(trades, 0);
    EXPECT_TRUE(book.best_bid().has_value());
}

TEST(Matcher, AggressorRestsResidualAfterPartialMatch) {
    OrderBook book("SYM");
    book.rest(mk(1, Side::Sell, 10000, 100));
    Matcher m(MatchAlgo::ProRata);
    EventList out;
    m.submit(book, mk(99, Side::Buy, 10000, 300), out);
    ASSERT_TRUE(book.best_bid().has_value());
    EXPECT_EQ(*book.best_bid(), 10000);
}

TEST(Matcher, SellAggressorWalksBids) {
    OrderBook book("SYM");
    book.rest(mk(1, Side::Buy, 10001, 100));
    book.rest(mk(2, Side::Buy, 10000, 100));
    Matcher m(MatchAlgo::ProRata);
    EventList out;
    m.submit(book, mk(99, Side::Sell, 10000, 150), out);
    EXPECT_EQ(fills_for(out, 1), 100u);
    EXPECT_EQ(fills_for(out, 2), 50u);
}

TEST(Matcher, AckBeforeTrades) {
    OrderBook book("SYM");
    book.rest(mk(1, Side::Sell, 10000, 100));
    Matcher m(MatchAlgo::ProRata);
    EventList out;
    m.submit(book, mk(99, Side::Buy, 10000, 100), out);
    ASSERT_FALSE(out.empty());
    EXPECT_TRUE(std::holds_alternative<Ack>(out[0]));
}

TEST(Matcher, DegenerateSingleOrderEquivalence) {
    // With one resting order, pro-rata and FIFO must produce identical output.
    auto run = [](MatchAlgo a) {
        OrderBook book("SYM");
        book.rest(mk(1, Side::Sell, 10000, 100));
        Matcher m(a);
        EventList out;
        m.submit(book, mk(99, Side::Buy, 10000, 60), out);
        return fills_for(out, 1);
    };
    EXPECT_EQ(run(MatchAlgo::ProRata), run(MatchAlgo::Fifo));
}
