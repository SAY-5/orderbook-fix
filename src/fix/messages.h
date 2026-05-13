// SPDX-License-Identifier: MIT
#pragma once

#include "../core/types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace obfix::fix {

// FIX 4.4 tag numbers used by this engine. Tags not in this list are
// either silently ignored (admin tags we do not act on) or rejected by
// the parser/session as appropriate. See docs/fix-session.md.
namespace tag {
constexpr int BeginString = 8;
constexpr int BodyLength = 9;
constexpr int MsgType = 35;
constexpr int SenderCompID = 49;
constexpr int TargetCompID = 56;
constexpr int MsgSeqNum = 34;
constexpr int SendingTime = 52;
constexpr int CheckSum = 10;
constexpr int HeartBtInt = 108;
constexpr int TestReqID = 112;
constexpr int RefSeqNum = 45;
constexpr int Text = 58;
constexpr int BeginSeqNo = 7;
constexpr int EndSeqNo = 16;

// Order entry tags
constexpr int ClOrdID = 11;
constexpr int OrigClOrdID = 41;
constexpr int Symbol = 55;
constexpr int Side = 54;
constexpr int OrdType = 40;
constexpr int OrderQty = 38;
constexpr int Price = 44;
constexpr int TransactTime = 60;

// ExecutionReport tags
constexpr int OrderID = 37;
constexpr int ExecID = 17;
constexpr int ExecType = 150;
constexpr int OrdStatus = 39;
constexpr int LeavesQty = 151;
constexpr int CumQty = 14;
constexpr int AvgPx = 6;
constexpr int LastPx = 31;
constexpr int LastQty = 32;
}  // namespace tag

// Raw parsed FIX message: a tag->value multimap kept ordered for tests.
struct Message {
    // Single-value lookup (FIX repeating groups are not used by this engine).
    std::unordered_map<int, std::string> fields;

    std::optional<std::string> get(int t) const {
        auto it = fields.find(t);
        if (it == fields.end()) return std::nullopt;
        return it->second;
    }

    std::string msg_type() const {
        auto it = fields.find(tag::MsgType);
        return it == fields.end() ? std::string{} : it->second;
    }

    SeqNum seq() const {
        auto it = fields.find(tag::MsgSeqNum);
        if (it == fields.end()) return 0;
        return static_cast<SeqNum>(std::stoull(it->second));
    }
};

// MsgType code constants per FIX 4.4 admin + order entry subset.
namespace msgtype {
constexpr const char* Heartbeat = "0";
constexpr const char* TestRequest = "1";
constexpr const char* ResendRequest = "2";
constexpr const char* Reject = "3";
constexpr const char* SequenceReset = "4";
constexpr const char* Logout = "5";
constexpr const char* ExecutionReport = "8";
constexpr const char* Logon = "A";
constexpr const char* NewOrderSingle = "D";
constexpr const char* OrderCancelRequest = "F";
constexpr const char* OrderCancelReplace = "G";
}  // namespace msgtype

}  // namespace obfix::fix
