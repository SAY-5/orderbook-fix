// SPDX-License-Identifier: MIT
//
// ResendRequest fulfilment tests. Verify the session:
//   1. Retains outbound messages up to `outbound_history_size`.
//   2. Replays the requested seq range with PossDupFlag=Y and the
//      original SendingTime preserved in OrigSendingTime (122).
//   3. Falls back to SequenceReset-GapFill (35=4, 123=Y, 36=...) for
//      seqs that have fallen out of the bounded ring.

#include "fix/messages.h"
#include "fix/parser.h"
#include "fix/serializer.h"
#include "fix/session.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace obfix::fix;

namespace {

std::string wire(const std::vector<std::pair<int, std::string>>& body) {
    Serializer s(kSohText);
    return s.build(body);
}

Session make_session(std::size_t history_cap = 10000) {
    SessionConfig cfg;
    cfg.sender_comp_id = "OBFIX";
    cfg.target_comp_id = "CLIENT";
    cfg.heartbeat_secs = 30;
    cfg.outbound_history_size = history_cap;
    Session s(cfg, kSohText);
    s.set_now_provider([]() { return std::string("20260513-00:00:00.000"); });
    return s;
}

}  // namespace

// Drive the session through Logon + N TestRequests so the outbound history
// has N+1 entries (Logon echo + N Heartbeats with TestReqID). Then send a
// ResendRequest for the last 50 outbound seqs and verify the replays.
TEST(FixSessionResend, ReplaysLast50WithPossDupFlag) {
    Session s = make_session();
    // Logon: outbound seq 1 (the echoed Logon).
    s.on_bytes(wire({{35, "A"}, {49, "C"}, {56, "OBFIX"}, {34, "1"}, {108, "30"}}));
    EXPECT_EQ(s.out_seq(), 1u);

    // Send 100 TestRequests so the session emits 100 Heartbeats. Outbound
    // seq advances 1 -> 101.
    for (int i = 0; i < 100; ++i) {
        std::string trid = "ping-" + std::to_string(i);
        std::string in =
            wire({{35, "1"}, {49, "C"}, {56, "OBFIX"}, {34, std::to_string(i + 2)}, {112, trid}});
        s.on_bytes(in);
    }
    EXPECT_EQ(s.out_seq(), 101u);
    EXPECT_EQ(s.history_size(), 101u);

    // Ask for seq 52..101 (last 50).
    auto rr = wire({{35, "2"}, {49, "C"}, {56, "OBFIX"}, {34, "102"}, {7, "52"}, {16, "101"}});
    auto step = s.on_bytes(rr);
    EXPECT_FALSE(step.disconnect);
    EXPECT_EQ(step.out.size(), 50u);

    Parser p(kSohText);
    int with_possdup = 0;
    int heartbeats = 0;
    for (const auto& w : step.out) {
        ParseResult pr = p.parse_one(w.bytes);
        ASSERT_EQ(pr.err, ParseError::None);
        if (pr.msg.get(tag::PossDupFlag).value_or("") == "Y") ++with_possdup;
        if (pr.msg.msg_type() == "0") ++heartbeats;
        EXPECT_FALSE(pr.msg.get(tag::OrigSendingTime).value_or("").empty());
    }
    EXPECT_EQ(with_possdup, 50);
    EXPECT_EQ(heartbeats, 50);
}

// EndSeqNo=0 in FIX 4.4 means "to current end of stream". Verify the
// session interprets it that way.
TEST(FixSessionResend, EndSeqZeroMeansToCurrentEnd) {
    Session s = make_session();
    s.on_bytes(wire({{35, "A"}, {49, "C"}, {56, "OBFIX"}, {34, "1"}, {108, "30"}}));
    for (int i = 0; i < 5; ++i) {
        s.on_bytes(
            wire({{35, "1"}, {49, "C"}, {56, "OBFIX"}, {34, std::to_string(i + 2)}, {112, "p"}}));
    }
    EXPECT_EQ(s.out_seq(), 6u);
    auto step =
        s.on_bytes(wire({{35, "2"}, {49, "C"}, {56, "OBFIX"}, {34, "7"}, {7, "1"}, {16, "0"}}));
    EXPECT_EQ(step.out.size(), 6u);  // seq 1..6
}

// Seqs older than the retained ring are answered with SequenceReset-
// GapFill (35=4, 123=Y, 36=next). We cap the ring at 10 and request
// from seq 1 to 100; expect one GapFill covering 1..N + (101-N) replays.
TEST(FixSessionResend, OlderThanRingGapFills) {
    Session s = make_session(/*history_cap=*/10);
    s.on_bytes(wire({{35, "A"}, {49, "C"}, {56, "OBFIX"}, {34, "1"}, {108, "30"}}));
    // Generate 30 outbound; ring keeps only the last 10 (seq 21..30).
    for (int i = 0; i < 29; ++i) {
        s.on_bytes(
            wire({{35, "1"}, {49, "C"}, {56, "OBFIX"}, {34, std::to_string(i + 2)}, {112, "p"}}));
    }
    EXPECT_EQ(s.out_seq(), 30u);
    EXPECT_EQ(s.history_size(), 10u);

    // Request 5..30. Expect 1 GapFill (covering 5..20) + 10 replays (21..30).
    auto step =
        s.on_bytes(wire({{35, "2"}, {49, "C"}, {56, "OBFIX"}, {34, "31"}, {7, "5"}, {16, "30"}}));
    ASSERT_EQ(step.out.size(), 11u);
    Parser p(kSohText);
    // First out: GapFill.
    ParseResult pr0 = p.parse_one(step.out[0].bytes);
    ASSERT_EQ(pr0.err, ParseError::None);
    EXPECT_EQ(pr0.msg.msg_type(), "4");
    EXPECT_EQ(pr0.msg.get(tag::GapFillFlag).value_or(""), "Y");
    EXPECT_EQ(pr0.msg.get(tag::NewSeqNo).value_or(""), "21");
    EXPECT_EQ(pr0.msg.get(tag::MsgSeqNum).value_or(""), "5");
    // Remaining 10: PossDup replays seq 21..30.
    for (int i = 1; i <= 10; ++i) {
        ParseResult pr = p.parse_one(step.out[static_cast<std::size_t>(i)].bytes);
        ASSERT_EQ(pr.err, ParseError::None);
        EXPECT_EQ(pr.msg.get(tag::PossDupFlag).value_or(""), "Y");
        EXPECT_EQ(pr.msg.msg_type(), "0");  // Heartbeat replay
    }
}

// Disabling history (size 0) means resend requests can only emit a
// single GapFill that bumps the peer past the unretained range.
TEST(FixSessionResend, ZeroHistoryGapFillsEverything) {
    Session s = make_session(/*history_cap=*/0);
    s.on_bytes(wire({{35, "A"}, {49, "C"}, {56, "OBFIX"}, {34, "1"}, {108, "30"}}));
    for (int i = 0; i < 5; ++i) {
        s.on_bytes(
            wire({{35, "1"}, {49, "C"}, {56, "OBFIX"}, {34, std::to_string(i + 2)}, {112, "p"}}));
    }
    EXPECT_EQ(s.out_seq(), 6u);
    EXPECT_EQ(s.history_size(), 0u);
    auto step =
        s.on_bytes(wire({{35, "2"}, {49, "C"}, {56, "OBFIX"}, {34, "7"}, {7, "1"}, {16, "0"}}));
    ASSERT_EQ(step.out.size(), 1u);
    Parser p(kSohText);
    auto pr = p.parse_one(step.out[0].bytes);
    ASSERT_EQ(pr.err, ParseError::None);
    EXPECT_EQ(pr.msg.msg_type(), "4");
    EXPECT_EQ(pr.msg.get(tag::GapFillFlag).value_or(""), "Y");
    EXPECT_EQ(pr.msg.get(tag::NewSeqNo).value_or(""), "7");
}
