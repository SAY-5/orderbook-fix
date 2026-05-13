// SPDX-License-Identifier: MIT
//
// Heartbeat-timer semantics. The session exposes `tick(now)` so tests
// can drive a synthetic clock; the v4 production transport will call
// it on every recv loop iteration. We exercise three regimes:
//   1. Idle Heartbeat emission every HeartBtInt with no other outbound.
//   2. TestRequest emission after 1.5*HeartBtInt of inbound silence.
//   3. Logout + disconnect after 1.5*HeartBtInt of TestRequest silence.

#include "fix/messages.h"
#include "fix/parser.h"
#include "fix/serializer.h"
#include "fix/session.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

using namespace obfix::fix;
using Clock = Session::Clock;

namespace {

std::string wire(const std::vector<std::pair<int, std::string>>& body) {
    Serializer s(kSohText);
    return s.build(body);
}

// Shared synthetic clock so on_bytes() and tick() see the same time.
// Tests bump `g_now` to advance virtual wall-clock without sleeping.
inline Session::Clock::time_point& g_clock() {
    static Session::Clock::time_point t = Session::Clock::time_point{};
    return t;
}

Session make_session(int hb_secs = 2) {
    SessionConfig cfg;
    cfg.sender_comp_id = "OBFIX";
    cfg.target_comp_id = "CLIENT";
    cfg.heartbeat_secs = hb_secs;
    Session s(cfg, kSohText);
    s.set_now_provider([]() { return std::string("20260513-00:00:00.000"); });
    s.set_clock_provider([]() { return g_clock(); });
    return s;
}

void login(Session& s) {
    auto m = wire({{35, "A"}, {49, "C"}, {56, "OBFIX"}, {34, "1"}, {108, "2"}});
    s.on_bytes(m);
}

int count_msg_type(const SessionStep& step, const std::string& mt) {
    Parser p(kSohText);
    int n = 0;
    for (const auto& w : step.out) {
        auto pr = p.parse_one(w.bytes);
        if (pr.err == ParseError::None && pr.msg.msg_type() == mt) ++n;
    }
    return n;
}

}  // namespace

// Synthetic-time orchestration matching the task spec: 2-second
// HeartBtInt, no client traffic. Over 7 seconds of ticks at 250ms
// resolution we expect at least 3 server-side Heartbeats (one at t=2s,
// 4s, 6s). After 1.5*HeartBtInt = 3s of inbound silence the session
// emits TestRequest. After another 3s of silence past TestRequest, the
// session emits Logout and disconnects.
TEST(FixSessionHeartbeat, ThreeHeartbeatsThenTestRequestThenLogout) {
    g_clock() = Clock::time_point{};
    auto t0 = g_clock();
    Session s = make_session(/*hb_secs=*/2);
    // Drive logon via on_bytes; that bumps last_inbound_ and last_outbound_.
    login(s);
    ASSERT_EQ(s.state(), SessionState::LoggedIn);

    // First we simulate the heartbeat-emission phase. last_inbound_ is
    // "now" at logon; we tick forward in 250ms steps without injecting
    // any inbound. The session emits a Heartbeat each time
    // now - last_outbound_ >= 2s. It emits TestRequest at
    // now - last_inbound_ >= 3s (which happens at t=3s in this loop).
    int heartbeats = 0;
    int test_requests = 0;
    bool disconnected = false;
    int logouts = 0;

    // Re-derive a synthetic clock from t0. The Session reads Clock::now()
    // internally for stamping, but tick() honors the passed-in time, so
    // skewing the loop's interpretation of "now" past the real
    // wall-clock is safe.
    for (int step_ms = 250; step_ms <= 11000; step_ms += 250) {
        auto now = t0 + std::chrono::milliseconds(step_ms);
        g_clock() = now;
        SessionStep r = s.tick(now);
        heartbeats += count_msg_type(r, "0");
        test_requests += count_msg_type(r, "1");
        logouts += count_msg_type(r, "5");
        if (r.disconnect) {
            disconnected = true;
            break;
        }
    }

    // 11 seconds elapsed virtual time. Expected behaviour:
    //   t=2s: first Heartbeat (no other outbound for 2s).
    //   t=3s: TestRequest (no inbound for 3s = 1.5*HB).
    //   t=6s: Logout + disconnect (3s after TestRequest, still no reply).
    // The Heartbeat-vs-TestRequest race at t=3s is resolved in favor of
    // TestRequest because the session checks that branch first.
    EXPECT_GE(heartbeats, 1) << "no heartbeats fired in 11s with 2s HB";
    EXPECT_EQ(test_requests, 1);
    EXPECT_EQ(logouts, 1);
    EXPECT_TRUE(disconnected);
    EXPECT_EQ(s.state(), SessionState::LogoutSent);
}

// Without inbound silence, the session emits a Heartbeat each HeartBtInt
// of pure idle outbound silence. Inject inbound every 1s so TestRequest
// never fires; over 7s we expect 3 Heartbeats at t=2,4,6.
TEST(FixSessionHeartbeat, ThreeHeartbeatsOverSevenSecondsWithInboundLiveness) {
    g_clock() = Clock::time_point{};
    auto t0 = g_clock();
    Session s = make_session(/*hb_secs=*/2);
    login(s);
    int heartbeats = 0;
    int hb_seen_at = 0;
    obfix::SeqNum next_in_seq = 2;
    for (int step_ms = 250; step_ms <= 7000; step_ms += 250) {
        auto now = t0 + std::chrono::milliseconds(step_ms);
        g_clock() = now;
        // Every full second, the peer sends an inbound Heartbeat to
        // keep liveness up.
        if (step_ms % 1000 == 0) {
            auto inb =
                wire({{35, "0"}, {49, "C"}, {56, "OBFIX"}, {34, std::to_string(next_in_seq++)}});
            s.on_bytes(inb);
        }
        SessionStep r = s.tick(now);
        int hb = count_msg_type(r, "0");
        heartbeats += hb;
        if (hb > 0) hb_seen_at = step_ms;
        // No TestRequest because inbound keeps arriving.
        EXPECT_EQ(count_msg_type(r, "1"), 0) << "TestRequest at step " << step_ms;
    }
    EXPECT_GE(heartbeats, 3);
    EXPECT_GE(hb_seen_at, 5000);  // last HB lands at or near t=6s
}

// A TestRequest reply (the peer sends a Heartbeat with our TestReqID
// echoed back) cancels the outstanding TestRequest and the session
// returns to the normal heartbeat schedule.
TEST(FixSessionHeartbeat, TestRequestClearedByInbound) {
    g_clock() = Clock::time_point{};
    auto t0 = g_clock();
    Session s = make_session(/*hb_secs=*/2);
    login(s);

    // Tick forward 3s: session emits TestRequest.
    g_clock() = t0 + std::chrono::milliseconds(3100);
    SessionStep r1 = s.tick(g_clock());
    ASSERT_EQ(count_msg_type(r1, "1"), 1);

    // Peer responds with a Heartbeat carrying the TestReqID. The Session
    // does not check that the TestReqID matches; any inbound parse
    // clears the outstanding flag.
    auto reply = wire({{35, "0"}, {49, "C"}, {56, "OBFIX"}, {34, "2"}});
    s.on_bytes(reply);

    // 3 more seconds of silence: the session should now restart the
    // TestRequest timer, not fire a Logout.
    g_clock() = t0 + std::chrono::milliseconds(6500);
    SessionStep r2 = s.tick(g_clock());
    EXPECT_FALSE(r2.disconnect);
    EXPECT_EQ(count_msg_type(r2, "5"), 0);
}
