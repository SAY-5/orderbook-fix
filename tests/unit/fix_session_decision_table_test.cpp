// SPDX-License-Identifier: MIT
//
// FIX session state-machine decision table. Each row enumerates a (state,
// inbound MsgType) pair and asserts the resulting (next_state, out_msgs,
// disconnect) tuple. The table is exhaustive over the implemented
// MsgTypes; admin types we do not act on (SequenceReset) are included so
// the no-op cases stay pinned.

#include "fix/messages.h"
#include "fix/parser.h"
#include "fix/serializer.h"
#include "fix/session.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

using namespace obfix::fix;

namespace {

std::string wire(const std::vector<std::pair<int, std::string>>& body) {
    Serializer s(kSohText);
    return s.build(body);
}

Session fresh() {
    SessionConfig cfg;
    cfg.sender_comp_id = "OBFIX";
    cfg.target_comp_id = "CLIENT";
    cfg.heartbeat_secs = 30;
    Session s(cfg, kSohText);
    s.set_now_provider([]() { return std::string("20260513-00:00:00.000"); });
    return s;
}

Session logged_in(obfix::SeqNum start_seq = 1) {
    Session s = fresh();
    auto m = wire({{35, "A"},
                   {49, "CLIENT"},
                   {56, "OBFIX"},
                   {34, std::to_string(start_seq)},
                   {52, "x"},
                   {108, "30"}});
    s.on_bytes(m);
    return s;
}

// One row of the decision table. We store the inbound bytes (already
// serialized with the right seq number) plus the expected effects.
struct Row {
    const char* name;
    SessionState entry;
    std::string inbound;
    SessionState exit;
    bool expect_disconnect;
    int expect_out_count;
    std::optional<std::string> expect_first_msg_type;  // if out_count > 0
    int expect_app_count;
};

}  // namespace

class FixSessionDecisionTable : public ::testing::TestWithParam<Row> {};

TEST_P(FixSessionDecisionTable, Row) {
    const auto& row = GetParam();
    Session s = (row.entry == SessionState::Disconnected) ? fresh() : logged_in();
    ASSERT_EQ(s.state(), row.entry) << row.name;

    SessionStep step = s.on_bytes(row.inbound);
    EXPECT_EQ(s.state(), row.exit) << row.name;
    EXPECT_EQ(step.disconnect, row.expect_disconnect) << row.name;
    EXPECT_EQ(static_cast<int>(step.out.size()), row.expect_out_count) << row.name;
    EXPECT_EQ(static_cast<int>(step.app.size()), row.expect_app_count) << row.name;
    if (row.expect_first_msg_type && !step.out.empty()) {
        Parser p(kSohText);
        ParseResult pr = p.parse_one(step.out[0].bytes);
        ASSERT_EQ(pr.err, ParseError::None) << row.name;
        EXPECT_EQ(pr.msg.msg_type(), *row.expect_first_msg_type) << row.name;
    }
}

INSTANTIATE_TEST_SUITE_P(
    DecisionTable, FixSessionDecisionTable,
    ::testing::Values(
        // ---- Disconnected entry ----
        Row{"Disconnected+Logon", SessionState::Disconnected,
            wire({{35, "A"}, {49, "CLIENT"}, {56, "OBFIX"}, {34, "1"}, {108, "30"}}),
            SessionState::LoggedIn, false, 1, std::string("A"), 0},
        Row{"Disconnected+Heartbeat_disconnects", SessionState::Disconnected,
            wire({{35, "0"}, {49, "CLIENT"}, {56, "OBFIX"}, {34, "1"}}), SessionState::Disconnected,
            true, 0, std::nullopt, 0},
        Row{"Disconnected+TestRequest_disconnects", SessionState::Disconnected,
            wire({{35, "1"}, {49, "CLIENT"}, {56, "OBFIX"}, {34, "1"}, {112, "x"}}),
            SessionState::Disconnected, true, 0, std::nullopt, 0},
        Row{"Disconnected+ResendRequest_disconnects", SessionState::Disconnected,
            wire({{35, "2"}, {49, "CLIENT"}, {56, "OBFIX"}, {34, "1"}, {7, "1"}, {16, "0"}}),
            SessionState::Disconnected, true, 0, std::nullopt, 0},
        Row{"Disconnected+Logout_disconnects", SessionState::Disconnected,
            wire({{35, "5"}, {49, "CLIENT"}, {56, "OBFIX"}, {34, "1"}}), SessionState::Disconnected,
            true, 0, std::nullopt, 0},
        Row{"Disconnected+NewOrder_disconnects", SessionState::Disconnected,
            wire({{35, "D"}, {49, "CLIENT"}, {56, "OBFIX"}, {34, "1"}, {11, "x"}}),
            SessionState::Disconnected, true, 0, std::nullopt, 0},
        Row{"Disconnected+Cancel_disconnects", SessionState::Disconnected,
            wire({{35, "F"}, {49, "CLIENT"}, {56, "OBFIX"}, {34, "1"}, {41, "x"}, {11, "y"}}),
            SessionState::Disconnected, true, 0, std::nullopt, 0},
        Row{"Disconnected+CancelReplace_disconnects", SessionState::Disconnected,
            wire({{35, "G"}, {49, "CLIENT"}, {56, "OBFIX"}, {34, "1"}, {41, "x"}, {11, "y"}}),
            SessionState::Disconnected, true, 0, std::nullopt, 0},
        Row{"Disconnected+UnknownType_disconnects", SessionState::Disconnected,
            wire({{35, "R"}, {49, "CLIENT"}, {56, "OBFIX"}, {34, "1"}}), SessionState::Disconnected,
            true, 0, std::nullopt, 0},

        // ---- LoggedIn entry, sequenced ----
        Row{"LoggedIn+Heartbeat_noop", SessionState::LoggedIn,
            wire({{35, "0"}, {49, "CLIENT"}, {56, "OBFIX"}, {34, "2"}}), SessionState::LoggedIn,
            false, 0, std::nullopt, 0},
        Row{"LoggedIn+TestRequest_repliesHeartbeat", SessionState::LoggedIn,
            wire({{35, "1"}, {49, "CLIENT"}, {56, "OBFIX"}, {34, "2"}, {112, "ping"}}),
            SessionState::LoggedIn, false, 1, std::string("0"), 0},
        Row{"LoggedIn+ResendRequest_logoutAndDisconnect", SessionState::LoggedIn,
            wire({{35, "2"}, {49, "CLIENT"}, {56, "OBFIX"}, {34, "2"}, {7, "1"}, {16, "0"}}),
            SessionState::LogoutSent, true, 1, std::string("5"), 0},
        Row{"LoggedIn+Logout_echoAndDisconnect", SessionState::LoggedIn,
            wire({{35, "5"}, {49, "CLIENT"}, {56, "OBFIX"}, {34, "2"}}), SessionState::Disconnected,
            true, 1, std::string("5"), 0},
        Row{"LoggedIn+SequenceReset_noop", SessionState::LoggedIn,
            wire({{35, "4"}, {49, "CLIENT"}, {56, "OBFIX"}, {34, "2"}}), SessionState::LoggedIn,
            false, 0, std::nullopt, 0},
        Row{"LoggedIn+Reject_noop", SessionState::LoggedIn,
            wire({{35, "3"}, {49, "CLIENT"}, {56, "OBFIX"}, {34, "2"}, {45, "1"}, {58, "x"}}),
            SessionState::LoggedIn, false, 0, std::nullopt, 0},
        Row{"LoggedIn+NewOrder_dispatchApp", SessionState::LoggedIn,
            wire({{35, "D"},
                  {49, "CLIENT"},
                  {56, "OBFIX"},
                  {34, "2"},
                  {11, "o-1"},
                  {55, "SYM"},
                  {54, "1"},
                  {38, "100"},
                  {44, "10000"}}),
            SessionState::LoggedIn, false, 0, std::nullopt, 1},
        Row{"LoggedIn+CancelRequest_dispatchApp", SessionState::LoggedIn,
            wire({{35, "F"},
                  {49, "CLIENT"},
                  {56, "OBFIX"},
                  {34, "2"},
                  {41, "o-1"},
                  {11, "c-1"},
                  {55, "SYM"},
                  {54, "1"}}),
            SessionState::LoggedIn, false, 0, std::nullopt, 1},
        Row{"LoggedIn+CancelReplace_dispatchApp", SessionState::LoggedIn,
            wire({{35, "G"},
                  {49, "CLIENT"},
                  {56, "OBFIX"},
                  {34, "2"},
                  {41, "o-1"},
                  {11, "r-1"},
                  {55, "SYM"},
                  {54, "1"},
                  {38, "200"},
                  {44, "10001"}}),
            SessionState::LoggedIn, false, 0, std::nullopt, 1},
        Row{"LoggedIn+UnknownType_reject", SessionState::LoggedIn,
            wire({{35, "R"}, {49, "CLIENT"}, {56, "OBFIX"}, {34, "2"}}), SessionState::LoggedIn,
            false, 1, std::string("3"), 0},
        Row{"LoggedIn+HighSeq_resendRequest", SessionState::LoggedIn,
            wire({{35, "0"}, {49, "CLIENT"}, {56, "OBFIX"}, {34, "5"}}), SessionState::LoggedIn,
            false, 1, std::string("2"), 0},
        Row{"LoggedIn+ExecutionReportInbound_reject_unknownAppType", SessionState::LoggedIn,
            wire({{35, "8"}, {49, "CLIENT"}, {56, "OBFIX"}, {34, "2"}, {37, "x"}}),
            SessionState::LoggedIn, false, 1, std::string("3"), 0}));
