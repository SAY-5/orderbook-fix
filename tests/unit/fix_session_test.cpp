// SPDX-License-Identifier: MIT
#include "fix/session.h"
#include "fix/serializer.h"

#include <gtest/gtest.h>

using namespace obfix::fix;

namespace {
std::string wire(const std::vector<std::pair<int, std::string>>& body) {
    Serializer s(kSohText);
    return s.build(body);
}

Session make_session() {
    SessionConfig cfg;
    cfg.sender_comp_id = "OBFIX";
    cfg.target_comp_id = "CLIENT";
    cfg.heartbeat_secs = 30;
    Session sess(cfg, kSohText);
    sess.set_now_provider([]() { return std::string("20260513-00:00:00.000"); });
    return sess;
}
}  // namespace

TEST(FixSession, StartsDisconnected) {
    Session s = make_session();
    EXPECT_EQ(s.state(), SessionState::Disconnected);
}

TEST(FixSession, LogonAdvancesState) {
    Session s = make_session();
    auto m = wire({{35, "A"}, {49, "CLIENT"}, {56, "OBFIX"}, {34, "1"}, {52, "x"}, {108, "30"}});
    auto step = s.on_bytes(m);
    EXPECT_EQ(s.state(), SessionState::LoggedIn);
    EXPECT_FALSE(step.disconnect);
    ASSERT_EQ(step.out.size(), 1u);
    EXPECT_FALSE(step.out[0].bytes.empty());  // logon echo
}

TEST(FixSession, NonLogonFirstMessageDisconnects) {
    Session s = make_session();
    auto m = wire({{35, "0"}, {49, "CLIENT"}, {56, "OBFIX"}, {34, "1"}, {52, "x"}});
    auto step = s.on_bytes(m);
    EXPECT_TRUE(step.disconnect);
    EXPECT_EQ(s.state(), SessionState::Disconnected);
}

TEST(FixSession, LogonWithBadTargetCompDisconnects) {
    Session s = make_session();
    auto m = wire({{35, "A"}, {49, "CLIENT"}, {56, "WRONG"}, {34, "1"}, {52, "x"}, {108, "30"}});
    auto step = s.on_bytes(m);
    EXPECT_TRUE(step.disconnect);
    EXPECT_EQ(s.state(), SessionState::Disconnected);
}

TEST(FixSession, AcceptsInOrderSeqAfterLogon) {
    Session s = make_session();
    s.on_bytes(wire({{35, "A"}, {49, "C"}, {56, "OBFIX"}, {34, "1"}, {108, "30"}}));
    auto m = wire({{35, "D"},
                   {49, "C"},
                   {56, "OBFIX"},
                   {34, "2"},
                   {11, "x"},
                   {55, "S"},
                   {54, "1"},
                   {38, "100"},
                   {44, "1000"}});
    auto step = s.on_bytes(m);
    EXPECT_FALSE(step.disconnect);
    ASSERT_EQ(step.app.size(), 1u);
    EXPECT_EQ(step.app[0].msg.msg_type(), "D");
    EXPECT_EQ(s.in_seq(), 2u);
}

TEST(FixSession, GapTriggersResendRequest) {
    Session s = make_session();
    s.on_bytes(wire({{35, "A"}, {49, "C"}, {56, "OBFIX"}, {34, "1"}, {108, "30"}}));
    auto m = wire({{35, "0"}, {49, "C"}, {56, "OBFIX"}, {34, "5"}});  // skipped 2,3,4
    auto step = s.on_bytes(m);
    EXPECT_FALSE(step.disconnect);
    ASSERT_EQ(step.out.size(), 1u);
    Parser p(kSohText);
    auto pr = p.parse_one(step.out[0].bytes);
    ASSERT_EQ(pr.err, ParseError::None);
    EXPECT_EQ(pr.msg.msg_type(), "2");           // ResendRequest
    EXPECT_EQ(pr.msg.get(7).value_or(""), "2");  // BeginSeqNo
    EXPECT_EQ(s.in_seq(), 1u);                   // not advanced
}

TEST(FixSession, LowSeqNumDisconnects) {
    Session s = make_session();
    s.on_bytes(wire({{35, "A"}, {49, "C"}, {56, "OBFIX"}, {34, "5"}, {108, "30"}}));
    // After logon with reset_on_logon=true, in_seq = 5. Next seq 3 is low.
    auto m = wire({{35, "0"}, {49, "C"}, {56, "OBFIX"}, {34, "3"}});
    auto step = s.on_bytes(m);
    EXPECT_TRUE(step.disconnect);
}

TEST(FixSession, HeartbeatNoOp) {
    Session s = make_session();
    s.on_bytes(wire({{35, "A"}, {49, "C"}, {56, "OBFIX"}, {34, "1"}, {108, "30"}}));
    auto step = s.on_bytes(wire({{35, "0"}, {49, "C"}, {56, "OBFIX"}, {34, "2"}}));
    EXPECT_EQ(step.out.size(), 0u);
    EXPECT_EQ(step.app.size(), 0u);
    EXPECT_FALSE(step.disconnect);
}

TEST(FixSession, TestRequestProducesHeartbeatWithTestReqID) {
    Session s = make_session();
    s.on_bytes(wire({{35, "A"}, {49, "C"}, {56, "OBFIX"}, {34, "1"}, {108, "30"}}));
    auto step = s.on_bytes(wire({{35, "1"}, {49, "C"}, {56, "OBFIX"}, {34, "2"}, {112, "ping"}}));
    ASSERT_EQ(step.out.size(), 1u);
    Parser p(kSohText);
    auto pr = p.parse_one(step.out[0].bytes);
    ASSERT_EQ(pr.err, ParseError::None);
    EXPECT_EQ(pr.msg.msg_type(), "0");
    EXPECT_EQ(pr.msg.get(112).value_or(""), "ping");
}

TEST(FixSession, PeerLogoutEchoesAndDisconnects) {
    Session s = make_session();
    s.on_bytes(wire({{35, "A"}, {49, "C"}, {56, "OBFIX"}, {34, "1"}, {108, "30"}}));
    auto step = s.on_bytes(wire({{35, "5"}, {49, "C"}, {56, "OBFIX"}, {34, "2"}}));
    EXPECT_TRUE(step.disconnect);
    EXPECT_EQ(s.state(), SessionState::Disconnected);
    ASSERT_EQ(step.out.size(), 1u);
    Parser p(kSohText);
    auto pr = p.parse_one(step.out[0].bytes);
    ASSERT_EQ(pr.err, ParseError::None);
    EXPECT_EQ(pr.msg.msg_type(), "5");
}

TEST(FixSession, UnsupportedMsgTypeIsRejected) {
    Session s = make_session();
    s.on_bytes(wire({{35, "A"}, {49, "C"}, {56, "OBFIX"}, {34, "1"}, {108, "30"}}));
    // 35=R is QuoteRequest; not supported by this engine.
    auto step = s.on_bytes(wire({{35, "R"}, {49, "C"}, {56, "OBFIX"}, {34, "2"}}));
    EXPECT_EQ(step.app.size(), 0u);
    ASSERT_EQ(step.out.size(), 1u);
    Parser p(kSohText);
    auto pr = p.parse_one(step.out[0].bytes);
    ASSERT_EQ(pr.err, ParseError::None);
    EXPECT_EQ(pr.msg.msg_type(), "3");  // Reject
}

TEST(FixSession, MalformedFramingDisconnects) {
    Session s = make_session();
    // Long enough (>= 20 bytes) for the parser to commit to a verdict
    // rather than returning Incomplete and buffering.
    auto step = s.on_bytes("not a valid fix message at all, please reject this");
    EXPECT_TRUE(step.disconnect);
}

TEST(FixSession, OutSeqIncrementsOnEverySend) {
    Session s = make_session();
    auto a = s.send_heartbeat();
    auto b = s.send_heartbeat();
    Parser p(kSohText);
    auto pa = p.parse_one(a.bytes);
    auto pb = p.parse_one(b.bytes);
    ASSERT_EQ(pa.err, ParseError::None);
    ASSERT_EQ(pb.err, ParseError::None);
    EXPECT_EQ(std::stoull(pa.msg.get(34).value_or("0")), 1u);
    EXPECT_EQ(std::stoull(pb.msg.get(34).value_or("0")), 2u);
}

TEST(FixSession, ResendRequestReplaysFromOutboundHistoryWithPossDup) {
    // v3: the session retains an outbound history ring. ResendRequest
    // BeginSeqNo=1 EndSeqNo=0 replays everything from seq 1 to current,
    // marking each replay PossDupFlag=Y with the original SendingTime in
    // OrigSendingTime (122).
    Session s = make_session();
    s.on_bytes(wire({{35, "A"}, {49, "C"}, {56, "OBFIX"}, {34, "1"}, {108, "30"}}));
    auto step =
        s.on_bytes(wire({{35, "2"}, {49, "C"}, {56, "OBFIX"}, {34, "2"}, {7, "1"}, {16, "0"}}));
    EXPECT_FALSE(step.disconnect);
    ASSERT_EQ(step.out.size(), 1u);  // one Logon echo retained = one replay
    Parser p(kSohText);
    auto pr = p.parse_one(step.out[0].bytes);
    ASSERT_EQ(pr.err, ParseError::None);
    EXPECT_EQ(pr.msg.msg_type(), "A");  // replayed Logon
    EXPECT_EQ(pr.msg.get(tag::PossDupFlag).value_or(""), "Y");
    EXPECT_FALSE(pr.msg.get(tag::OrigSendingTime).value_or("").empty());
}

TEST(FixSession, MultipleMessagesInOneRecv) {
    Session s = make_session();
    auto logon = wire({{35, "A"}, {49, "C"}, {56, "OBFIX"}, {34, "1"}, {108, "30"}});
    auto order = wire({{35, "D"},
                       {49, "C"},
                       {56, "OBFIX"},
                       {34, "2"},
                       {11, "x"},
                       {38, "100"},
                       {44, "1000"},
                       {54, "1"}});
    auto step = s.on_bytes(logon + order);
    EXPECT_FALSE(step.disconnect);
    EXPECT_EQ(step.app.size(), 1u);
}
