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
    SeqNum assigned_seq = 0;
    if (!seen_seq) {
        ++out_seq_;
        assigned_seq = out_seq_;
        b.emplace_back(tag::MsgSeqNum, std::to_string(out_seq_));
    }
    std::string sending_time;
    if (!seen_time) {
        sending_time = default_now();
        b.emplace_back(tag::SendingTime, sending_time);
    }
    // Body fields after the standard header.
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (static_cast<int>(i) == msg_type_pos) continue;
        b.push_back(body[i]);
    }
    std::string bytes = serializer_.build(b);
    // Retain in outbound history so a future ResendRequest can replay it.
    // We only retain messages we own the seq for (assigned_seq != 0); the
    // caller is responsible for messages where they preset MsgSeqNum.
    if (assigned_seq != 0 && cfg_.outbound_history_size > 0) {
        // Recover the SendingTime from `b` if the caller passed it instead
        // of letting build_message assign now.
        std::string st = sending_time;
        if (st.empty()) {
            for (const auto& kv : b)
                if (kv.first == tag::SendingTime) {
                    st = kv.second;
                    break;
                }
        }
        retain_outbound(assigned_seq, st, bytes);
    }
    return WireOut{std::move(bytes)};
}

void Session::retain_outbound(SeqNum seq, const std::string& sending_time,
                              const std::string& bytes) {
    history_.push_back(OutboundRecord{seq, sending_time, bytes});
    while (history_.size() > cfg_.outbound_history_size) history_.pop_front();
}

void Session::handle_resend_request(SeqNum begin, SeqNum end, std::vector<WireOut>& out) {
    // Normalize the range. EndSeqNo=0 means "to current end of stream".
    if (end == 0) end = out_seq_;
    if (begin == 0 || begin > end || begin > out_seq_) return;

    // Find the oldest seq we still have in history_.
    SeqNum oldest_retained = history_.empty() ? out_seq_ + 1 : history_.front().seq;

    // For seqs that fell out of the ring, emit a SequenceReset-GapFill
    // (35=4, 123=Y, 36=<next-seq-to-restart-at>). We coalesce the entire
    // gap range into one GapFill message rather than emitting one per
    // missing seq; that is what FIX 4.4 prescribes for older retransmits.
    SeqNum cursor = begin;
    if (cursor < oldest_retained) {
        SeqNum new_seq_no = std::min(oldest_retained, end + 1);
        // The GapFill message itself carries the seq of `cursor` so the
        // peer accepts it as filling the gap from `cursor` to
        // new_seq_no - 1. We hand-build the wire bytes here so the
        // out_seq_ counter is not incremented by build_message (we are
        // replaying, not sending forward).
        std::vector<std::pair<int, std::string>> body{
            {tag::MsgType, msgtype::SequenceReset},
            {tag::SenderCompID, cfg_.sender_comp_id},
            {tag::TargetCompID, cfg_.target_comp_id},
            {tag::MsgSeqNum, std::to_string(cursor)},
            {tag::SendingTime, default_now()},
            {tag::GapFillFlag, "Y"},
            {tag::PossDupFlag, "Y"},
            {tag::NewSeqNo, std::to_string(new_seq_no)},
        };
        out.push_back(WireOut{serializer_.build(body)});
        cursor = new_seq_no;
    }
    if (cursor > end) return;

    // Walk the retained ring and replay each requested seq with
    // PossDupFlag=Y and OrigSendingTime=<original SendingTime>.
    for (const auto& rec : history_) {
        if (rec.seq < cursor) continue;
        if (rec.seq > end) break;
        // Re-parse the original bytes so we can re-serialize with the
        // PossDupFlag and OrigSendingTime tags spliced in. The parser is
        // tolerant of our own well-formed output, so this round-trips.
        ParseResult pr = parser_.parse_one(rec.bytes);
        if (pr.err != ParseError::None) continue;
        std::vector<std::pair<int, std::string>> body;
        body.reserve(pr.msg.fields.size() + 2);
        // MsgType first.
        auto mt_it = pr.msg.fields.find(tag::MsgType);
        if (mt_it != pr.msg.fields.end()) body.emplace_back(tag::MsgType, mt_it->second);
        // Copy header/body in a stable order, skipping fields the
        // serializer will re-emit (8/9/10) and the ones we are overriding.
        for (const auto& kv : pr.msg.fields) {
            int t = kv.first;
            if (t == tag::BeginString || t == tag::BodyLength || t == tag::CheckSum ||
                t == tag::MsgType || t == tag::PossDupFlag || t == tag::OrigSendingTime ||
                t == tag::SendingTime)
                continue;
            body.emplace_back(t, kv.second);
        }
        // FIX requires OrigSendingTime to equal the original SendingTime
        // when PossDupFlag=Y. Inject SendingTime "now" and OrigSendingTime
        // = retained.
        body.emplace_back(tag::SendingTime, default_now());
        body.emplace_back(tag::OrigSendingTime, rec.sending_time);
        body.emplace_back(tag::PossDupFlag, "Y");
        out.push_back(WireOut{serializer_.build(body)});
    }
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
        // FIX 4.4: BeginSeqNo = first seq the peer wants; EndSeqNo = last,
        // or 0 to mean "to current end". Messages still in `history_` are
        // replayed with PossDupFlag=Y; messages that fell out of the ring
        // are answered with SequenceReset-GapFill (35=4, 123=Y, 36=next).
        SeqNum begin = 0;
        SeqNum end = 0;
        if (auto b = m.get(tag::BeginSeqNo)) begin = static_cast<SeqNum>(std::stoull(*b));
        if (auto e = m.get(tag::EndSeqNo)) end = static_cast<SeqNum>(std::stoull(*e));
        handle_resend_request(begin, end, step.out);
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
