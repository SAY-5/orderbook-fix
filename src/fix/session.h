// SPDX-License-Identifier: MIT
#pragma once

#include "messages.h"
#include "parser.h"
#include "serializer.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace obfix::fix {

enum class SessionState {
    Disconnected,
    LogonSent,
    LoggedIn,
    LogoutSent,
};

struct SessionConfig {
    std::string sender_comp_id;  // local CompID
    std::string target_comp_id;  // peer CompID
    int heartbeat_secs{30};
    // The reset_on_logon flag tells the session whether to reset
    // in/out sequence numbers when a Logon is observed. Most production
    // sessions persist across reconnects; this engine is cold-start only
    // so the default is true.
    bool reset_on_logon{true};
    // Cap on the outbound history ring used to fulfil ResendRequest. The
    // session keeps the last `outbound_history_size` admin + app messages
    // it sent; older ones are answered with GapFill (35=4). 0 disables
    // history retention entirely.
    std::size_t outbound_history_size{10000};
};

// Outbound action a session wants the transport to perform. The session
// state machine is pure (no I/O), so it returns a list of WireOut chunks
// that the caller (TcpServer or test harness) writes to the socket.
struct WireOut {
    std::string bytes;
};

struct AppMessage {
    Message msg;
};

// Inbound classification: when the session is fed bytes, it splits them
// into wire output (admin replies) and application messages (NewOrderSingle
// etc.) that the matching engine should handle.
struct SessionStep {
    std::vector<WireOut> out;
    std::vector<AppMessage> app;
    bool disconnect{false};  // transport should close after writing `out`
};

class Session {
public:
    Session(SessionConfig cfg, char soh = kSohWire)
        : cfg_(std::move(cfg)), parser_(soh), serializer_(soh) {}

    SessionState state() const noexcept { return state_; }
    SeqNum in_seq() const noexcept { return in_seq_; }
    SeqNum out_seq() const noexcept { return out_seq_; }
    const SessionConfig& config() const noexcept { return cfg_; }

    // Active side: build a Logon and prepare to send it. Returns the
    // wire bytes the caller should write. Transitions Disconnected ->
    // LogonSent.
    WireOut initiate_logon();

    // Feed inbound bytes. Returns admin replies + app messages.
    SessionStep on_bytes(std::string_view data);

    // The caller (matching engine) finished processing an application
    // message and wants to send an ExecutionReport. The session
    // serializes it with the right seqnum, sender, target, and time.
    WireOut send_execution_report(const std::vector<std::pair<int, std::string>>& body);

    // Build and send a heartbeat. Called by the transport on the timer.
    WireOut send_heartbeat();

    // Build a Logout. After this, the session is in LogoutSent.
    WireOut send_logout(std::string_view text);

    // Test/diag setters.
    void reset_seq() {
        in_seq_ = 0;
        out_seq_ = 0;
    }
    void set_now_provider(std::function<std::string()> f) { now_ = std::move(f); }

    // Diagnostic accessor: count of outbound messages currently retained
    // in the resend history ring. Test-only.
    std::size_t history_size() const noexcept { return history_.size(); }

private:
    SessionStep handle_message(Message m);
    WireOut build_message(const std::vector<std::pair<int, std::string>>& body);
    WireOut build_resend_request(SeqNum from);
    WireOut build_reject(SeqNum ref, std::string_view reason);
    // Replay handler: walks `history_` for [begin, end] (end=0 means
    // "to current out_seq"), emitting either PossDupFlag=Y replays or
    // SequenceReset-GapFill (35=4 + GapFillFlag=Y + NewSeqNo=...) for
    // gaps that fell out of the ring.
    void handle_resend_request(SeqNum begin, SeqNum end, std::vector<WireOut>& out);
    void retain_outbound(SeqNum seq, const std::string& sending_time, const std::string& bytes);

    // One slot of outbound history. We retain just enough to rebuild a
    // PossDupFlag=Y replay; the original SendingTime goes into
    // OrigSendingTime (122) and the rest is the original wire bytes.
    struct OutboundRecord {
        SeqNum seq;
        std::string sending_time;
        std::string bytes;  // original on-the-wire bytes
    };

    std::string default_now() const;

    SessionConfig cfg_;
    Parser parser_;
    Serializer serializer_;
    SessionState state_{SessionState::Disconnected};
    SeqNum in_seq_{0};
    SeqNum out_seq_{0};
    std::string in_buf_;
    std::function<std::string()> now_;
    std::deque<OutboundRecord> history_;
};

}  // namespace obfix::fix
