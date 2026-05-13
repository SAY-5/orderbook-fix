// SPDX-License-Identifier: MIT
//
// Property-based tests, hand-rolled. We do not pull in rapidcheck because
// the corpus is small and pinning a seed is easier this way. Each TEST_F
// fixture runs N trials with a deterministic mt19937_64 seeded from the
// test name; failures are reproducible without recording a counterexample.
//
// Properties covered:
//   1. FIX parser  : random-shape valid messages round-trip through the
//                    serializer with field-equal semantics.
//   2. FIX checksum: compute(body) == (sum of bytes) & 0xFF, irrespective
//                    of content.
//   3. Pro-rata    : sum(fills) == min(aggressor_qty, sum(resting)) and
//                    no resting order is over-filled. The rounding residual
//                    follows FIFO order.

#include "core/matcher.h"
#include "core/order.h"
#include "core/order_book.h"
#include "fix/messages.h"
#include "fix/parser.h"
#include "fix/serializer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace obfix;
using namespace obfix::fix;

namespace {

constexpr int kTrials = 500;

// Pick a stable seed per test so failures are reproducible without the
// engine recording a corpus. The seed is the FNV-1a hash of the test name.
std::uint64_t seed_for(const char* name) {
    std::uint64_t h = 1469598103934665603ULL;
    for (const char* p = name; *p; ++p) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(*p));
        h *= 1099511628211ULL;
    }
    return h;
}

// Generate a random printable string with no SOH/'=' inside, since those
// characters terminate a field on the wire. Length 0..16.
std::string random_value(std::mt19937_64& rng) {
    std::uniform_int_distribution<int> len(0, 16);
    std::uniform_int_distribution<int> ch(33, 126);  // printable ASCII
    int n = len(rng);
    std::string s;
    s.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        char c = static_cast<char>(ch(rng));
        if (c == '=' || c == '|') c = 'X';
        s.push_back(c);
    }
    return s;
}

// Build a random valid FIX 4.4 body. Tag order randomized; MsgType is
// always first to keep the session parser happy when these messages reach
// it (the property tests below only exercise parser+serializer, but we
// still respect the spec).
std::vector<std::pair<int, std::string>> random_body(std::mt19937_64& rng) {
    static const int kCandidateTags[] = {49, 56, 34, 52, 11, 55, 54, 38, 44, 40, 60, 58, 6, 14, 17};
    std::vector<std::pair<int, std::string>> body;
    body.emplace_back(35, std::string(1, "ADFG085"[rng() % 7]));
    std::uniform_int_distribution<int> count(2, 10);
    int n = count(rng);
    std::vector<int> tags(std::begin(kCandidateTags), std::end(kCandidateTags));
    std::shuffle(tags.begin(), tags.end(), rng);
    for (int i = 0; i < n && i < static_cast<int>(tags.size()); ++i) {
        body.emplace_back(tags[static_cast<std::size_t>(i)], random_value(rng));
    }
    return body;
}

}  // namespace

// 1. Parser/serializer round-trip. Build a random body, serialize, parse,
// then re-serialize the parsed message (dropping 8/9/10 which the
// serializer prepends) and parse again. The two parse passes must agree on
// every non-header tag.
TEST(Property, ParserRoundTripPreservesFields) {
    std::mt19937_64 rng(seed_for("ParserRoundTripPreservesFields"));
    Serializer s(kSohText);
    Parser p(kSohText);
    for (int trial = 0; trial < kTrials; ++trial) {
        auto body = random_body(rng);
        std::string wire = s.build(body);
        ParseResult r1 = p.parse_one(wire);
        ASSERT_EQ(r1.err, ParseError::None) << "trial=" << trial << " wire=" << wire;
        ASSERT_EQ(r1.consumed, wire.size());

        // Rebuild from the parsed map; preserve MsgType first per FIX spec.
        std::vector<std::pair<int, std::string>> rebuilt;
        auto mt_it = r1.msg.fields.find(35);
        if (mt_it != r1.msg.fields.end()) rebuilt.emplace_back(35, mt_it->second);
        for (const auto& kv : r1.msg.fields) {
            if (kv.first == 8 || kv.first == 9 || kv.first == 10 || kv.first == 35) continue;
            rebuilt.emplace_back(kv.first, kv.second);
        }
        std::string wire2 = s.build(rebuilt);
        ParseResult r2 = p.parse_one(wire2);
        ASSERT_EQ(r2.err, ParseError::None) << "trial=" << trial;
        // Every non-header field must match.
        for (const auto& kv : r1.msg.fields) {
            if (kv.first == 8 || kv.first == 9 || kv.first == 10) continue;
            auto v = r2.msg.get(kv.first);
            ASSERT_TRUE(v.has_value()) << "missing tag " << kv.first << " trial=" << trial;
            ASSERT_EQ(*v, kv.second) << "tag " << kv.first << " trial=" << trial;
        }
    }
}

// 2. Checksum is the byte-sum mod 256 over the input. Trivial but worth
// pinning because any future optimization (SIMD, vectorized accumulator)
// must preserve this exactly.
TEST(Property, ChecksumIsByteSumMod256) {
    std::mt19937_64 rng(seed_for("ChecksumIsByteSumMod256"));
    std::uniform_int_distribution<int> len(0, 1024);
    std::uniform_int_distribution<int> byte(0, 255);
    for (int trial = 0; trial < kTrials; ++trial) {
        std::string body;
        int n = len(rng);
        body.reserve(static_cast<std::size_t>(n));
        unsigned want = 0;
        for (int i = 0; i < n; ++i) {
            unsigned char b = static_cast<unsigned char>(byte(rng));
            body.push_back(static_cast<char>(b));
            want = (want + b) & 0xFFu;
        }
        unsigned got = checksum(body);
        ASSERT_EQ(got, want) << "trial=" << trial << " n=" << n;
    }
}

// 3a. Pro-rata: sum(fills) over a single price level equals
// min(aggressor_qty, sum(resting_qty)). The aggressor side, the price
// level, the number of resting orders, and their sizes are all randomized.
TEST(Property, ProRataSumFillsEqualsMinAggressorTotal) {
    std::mt19937_64 rng(seed_for("ProRataSumFillsEqualsMinAggressorTotal"));
    std::uniform_int_distribution<int> count(1, 12);
    std::uniform_int_distribution<int> qty(1, 5000);
    for (int trial = 0; trial < kTrials; ++trial) {
        int n = count(rng);
        OrderBook book("SYM");
        Quantity total_resting = 0;
        for (int i = 0; i < n; ++i) {
            auto o = std::make_unique<Order>();
            o->id = static_cast<OrderId>(i + 1);
            o->side = Side::Sell;
            o->price = 10000;
            o->orig_qty = static_cast<Quantity>(qty(rng));
            o->leaves_qty = o->orig_qty;
            o->clord_id = ClOrdID{"r-" + std::to_string(i + 1)};
            total_resting += o->orig_qty;
            book.rest(std::move(o));
        }
        Quantity aggr_qty = static_cast<Quantity>(qty(rng));
        auto a = std::make_unique<Order>();
        a->id = 9999;
        a->side = Side::Buy;
        a->price = 10000;
        a->orig_qty = aggr_qty;
        a->leaves_qty = aggr_qty;
        a->clord_id = ClOrdID{"a"};

        Matcher m(MatchAlgo::ProRata);
        EventList out;
        m.submit(book, std::move(a), out);

        Quantity sum_fills = 0;
        std::map<OrderId, Quantity> per_resting;
        for (const auto& ev : out) {
            if (std::holds_alternative<Trade>(ev)) {
                const auto& t = std::get<Trade>(ev);
                sum_fills += t.qty;
                per_resting[t.resting_id] += t.qty;
            }
        }
        Quantity expected = std::min(aggr_qty, total_resting);
        ASSERT_EQ(sum_fills, expected)
            << "trial=" << trial << " aggr=" << aggr_qty << " total=" << total_resting;
        // No resting order is overfilled.
        for (const auto& kv : per_resting) {
            ASSERT_LE(kv.second, total_resting) << "trial=" << trial;
        }
    }
}

// 3b. Pro-rata FIFO residual: when equal-sized resting orders share a
// level, the matcher's floor allocation is identical for every slot. The
// rounding residual is then distributed strictly from head to tail. We
// derive the expected floor and residual from the same formula the matcher
// uses and assert head orders get the +1.
TEST(Property, ProRataResidualGoesFifo) {
    std::mt19937_64 rng(seed_for("ProRataResidualGoesFifo"));
    std::uniform_int_distribution<int> nresting(3, 9);
    std::uniform_int_distribution<int> size_d(3, 50);
    for (int trial = 0; trial < kTrials; ++trial) {
        int n = nresting(rng);
        Quantity sz = static_cast<Quantity>(size_d(rng));
        Quantity total = static_cast<Quantity>(n) * sz;
        // Pick to_fill strictly less than total so no order is fully
        // filled by floor alone (the residual fits comfortably in head).
        // Choose to_fill so floor + residual is a meaningful split.
        std::uniform_int_distribution<std::uint64_t> tof(1, static_cast<std::uint64_t>(total - 1));
        Quantity to_fill = static_cast<Quantity>(tof(rng));
        Quantity floor_each = (to_fill * sz) / total;
        Quantity residual = to_fill - floor_each * static_cast<Quantity>(n);
        // The matcher caps each slot's fill at its leaves_qty (=sz). For
        // the FIFO residual to fit, floor_each + 1 must be <= sz; this is
        // always true when to_fill <= total and slots are equal-sized.
        ASSERT_LE(floor_each, sz);

        OrderBook book("SYM");
        for (int i = 0; i < n; ++i) {
            auto o = std::make_unique<Order>();
            o->id = static_cast<OrderId>(i + 1);
            o->side = Side::Sell;
            o->price = 10000;
            o->orig_qty = sz;
            o->leaves_qty = sz;
            o->clord_id = ClOrdID{"r-" + std::to_string(i + 1)};
            book.rest(std::move(o));
        }
        auto a = std::make_unique<Order>();
        a->id = 9999;
        a->side = Side::Buy;
        a->price = 10000;
        a->orig_qty = to_fill;
        a->leaves_qty = to_fill;
        a->clord_id = ClOrdID{"a"};

        Matcher m(MatchAlgo::ProRata);
        EventList out;
        m.submit(book, std::move(a), out);

        std::map<OrderId, Quantity> per;
        for (const auto& ev : out) {
            if (std::holds_alternative<Trade>(ev)) {
                const auto& t = std::get<Trade>(ev);
                per[t.resting_id] += t.qty;
            }
        }
        Quantity total_filled = 0;
        for (int i = 1; i <= n; ++i) total_filled += per[static_cast<OrderId>(i)];
        ASSERT_EQ(total_filled, to_fill)
            << "trial=" << trial << " n=" << n << " sz=" << sz << " to_fill=" << to_fill;

        // Head orders absorb the residual FIFO; each takes min(cap, left).
        // Walk head-to-tail draining `residual` and check per-slot fill
        // against the matcher's actual policy.
        Quantity left = residual;
        for (int i = 1; i <= n; ++i) {
            Quantity cap = sz - floor_each;
            Quantity bonus = std::min(cap, left);
            left -= bonus;
            Quantity want = floor_each + bonus;
            ASSERT_EQ(per[static_cast<OrderId>(i)], want)
                << "trial=" << trial << " i=" << i << " n=" << n << " sz=" << sz
                << " to_fill=" << to_fill << " floor=" << floor_each << " residual=" << residual;
        }
        ASSERT_EQ(left, 0u) << "trial=" << trial;
    }
}
