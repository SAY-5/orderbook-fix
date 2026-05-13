// SPDX-License-Identifier: MIT
#include "session.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <utility>

namespace obfix::fix {

namespace {

std::string utc_now() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d:%02d:%02d.%03lld", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<long long>(ms));
    return std::string(buf);
}

}  // namespace

std::string Session::default_now() const {
    return now_ ? now_() : utc_now();
}

WireOut Session::build_message(const std::vector<std::pair<int, std::string>>& body) {
    // Inject SenderCompID, TargetCompID, MsgSeqNum, SendingTime if not
    // already present. The caller passes MsgType (35) first per FIX 4.4.
    std::vector<std::pair<int, std::string>> b;
    b.reserve(body.size() + 4);
    bool seen_sender = false, seen_target = false, seen_seq = false, seen_time = false;
    int msg_type_pos = -1;
    for (std::size_t i = 0; i < body.size(); ++i) {
        const auto& kv = body[i];
        if (kv.first == tag::MsgType) msg_type_pos = static_cast<int>(i);
        if (kv.first == tag::SenderCompID) seen_sender = true;
        if (kv.first == tag::TargetCompID) seen_target = true;
        if (kv.first == tag::MsgSeqNum) seen_seq = true;
        if (kv.first == tag::SendingTime) seen_time = true;
    }
    // Push MsgType first (FIX requires 35 immediately after 9).
    if (msg_type_pos >= 0) b.push_back(body[msg_type_pos]);
    if (!seen_sender) b.emplace_back(tag::SenderCompID, cfg_.sender_comp_id);
    if (!seen_target) b.emplace_back(tag::TargetCompID, cfg_.target_comp_id);
    if (!seen_seq) {
        ++out_seq_;
        b.emplace_back(tag::MsgSeqNum, std::to_string(out_seq_));
    }
    if (!seen_time) b.emplace_back(tag::SendingTime, default_now());
    // Body fields after the standard header.
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (static_cast<int>(i) == msg_type_pos) continue;
        b.push_back(body[i]);
    }
    return WireOut{serializer_.build(b)};
}

WireOut Session::initiate_logon() {
    state_ = SessionState::LogonSent;
    return build_message({
        {tag::MsgType, msgtype::Logon},
        {108, std::to_string(cfg_.heartbeat_secs)},
    });
}

WireOut Session::build_resend_request(SeqNum from) {
    return build_message({
        {tag::MsgType, msgtype::ResendRequest},
        {tag::BeginSeqNo, std::to_string(from)},
        {tag::EndSeqNo, "0"},  // 0 = up to infinity, per FIX 4.4
    });
}

WireOut Session::build_reject(SeqNum ref, std::string_view reason) {
    return build_message({
        {tag::MsgType, msgtype::Reject},
        {tag::RefSeqNum, std::to_string(ref)},
        {tag::Text, std::string(reason)},
    });
}

WireOut Session::send_heartbeat() {
    return build_message({{tag::MsgType, msgtype::Heartbeat}});
}

WireOut Session::send_logout(std::string_view text) {
    state_ = SessionState::LogoutSent;
    return build_message({
        {tag::MsgType, msgtype::Logout},
        {tag::Text, std::string(text)},
    });
}

WireOut Session::send_execution_report(const std::vector<std::pair<int, std::string>>& body) {
    // Caller passes the ExecutionReport-specific fields. We prepend 35=8.
    std::vector<std::pair<int, std::string>> b;
    b.reserve(body.size() + 1);
    b.emplace_back(tag::MsgType, msgtype::ExecutionReport);
    for (const auto& kv : body) b.push_back(kv);
    return build_message(b);
}

SessionStep Session::handle_message(Message m) {
    SessionStep step;
    const std::string mt = m.msg_type();
    SeqNum seq = m.seq();

    // Logon admission control.
    if (state_ == SessionState::Disconnected) {
        if (mt != msgtype::Logon) {
            step.disconnect = true;
            return step;
        }
        if (cfg_.reset_on_logon) {
            in_seq_ = 0;
            out_seq_ = 0;
        }
        // Validate TargetCompID matches our SenderCompID.
        auto target = m.get(tag::TargetCompID);
        auto sender = m.get(tag::SenderCompID);
        if (target && *target != cfg_.sender_comp_id) {
            step.disconnect = true;
            return step;
        }
        if (sender) {
            // Adopt the peer's SenderCompID as our TargetCompID. This lets
            // a test harness drive the session without configuring both
            // ends.
            cfg_.target_comp_id = *sender;
        }
        in_seq_ = seq;
        state_ = SessionState::LoggedIn;
        // Echo Logon. Per FIX, the acceptor's Logon reply mirrors HeartBtInt.
        step.out.push_back(build_message({
            {tag::MsgType, msgtype::Logon},
            {108, std::to_string(cfg_.heartbeat_secs)},
        }));
        return step;
    }

    // Gap detection: expected = in_seq_ + 1. We process the message only if
    // the sequence number matches. Higher: ResendRequest. Lower (and not
    // a PossDupFlag = Y, which this engine does not parse): disconnect.
    SeqNum expected = in_seq_ + 1;
    if (seq > expected) {
        step.out.push_back(build_resend_request(expected));
        // Do not advance in_seq_; the peer is expected to retransmit.
        // We still classify this message: drop it (no app dispatch).
        return step;
    }
    if (seq < expected && seq != 0) {
        // Per FIX, lower-than-expected without PossDupFlag is fatal.
        step.out.push_back(send_logout("MsgSeqNum too low"));
        step.disconnect = true;
        return step;
    }
    in_seq_ = seq;

    // Admin message dispatch.
    if (mt == msgtype::Heartbeat) {
        return step;  // nothing to do
    }
    if (mt == msgtype::TestRequest) {
        // Echo a Heartbeat with TestReqID.
        auto trid = m.get(tag::TestReqID).value_or("");
        step.out.push_back(build_message({
            {tag::MsgType, msgtype::Heartbeat},
            {tag::TestReqID, trid},
        }));
        return step;
    }
    if (mt == msgtype::ResendRequest) {
        // We do not retain history; reply with GapFill. This engine is
        // cold-start only (see README "What this is not").
        step.out.push_back(send_logout("resend not supported"));
        step.disconnect = true;
        return step;
    }
    if (mt == msgtype::Logout) {
        if (state_ != SessionState::LogoutSent) {
            // Peer-initiated logout: ack with our own Logout.
            step.out.push_back(send_logout("acknowledged"));
        }
        state_ = SessionState::Disconnected;
        step.disconnect = true;
        return step;
    }
    if (mt == msgtype::Reject || mt == msgtype::SequenceReset) {
        return step;
    }

    // Application messages: NewOrderSingle, OrderCancelRequest, OrderCancelReplace.
    if (mt == msgtype::NewOrderSingle || mt == msgtype::OrderCancelRequest ||
        mt == msgtype::OrderCancelReplace) {
        step.app.push_back(AppMessage{std::move(m)});
        return step;
    }

    // Unknown application MsgType: send Reject.
    step.out.push_back(build_reject(seq, std::string("unsupported MsgType=") + mt));
    return step;
}

SessionStep Session::on_bytes(std::string_view data) {
    in_buf_.append(data);
    SessionStep total;
    while (!in_buf_.empty()) {
        ParseResult pr = parser_.parse_one(in_buf_);
        if (pr.err == ParseError::Incomplete) break;
        if (pr.err != ParseError::None) {
            // Bad framing: send Logout and drop the connection. We do not
            // try to resynchronize; this matches what most production
            // counterparties do on a checksum mismatch.
            total.out.push_back(send_logout("malformed message"));
            total.disconnect = true;
            in_buf_.clear();
            return total;
        }
        in_buf_.erase(0, pr.consumed);
        SessionStep s = handle_message(std::move(pr.msg));
        for (auto& w : s.out) total.out.push_back(std::move(w));
        for (auto& a : s.app) total.app.push_back(std::move(a));
        if (s.disconnect) {
            total.disconnect = true;
            return total;
        }
    }
    return total;
}

}  // namespace obfix::fix
