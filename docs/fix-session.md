# FIX 4.4 session

This document covers the FIX 4.4 session state machine implemented in
`src/fix/session.{h,cpp}`. The engine acts as an acceptor (it does not
initiate logons against an upstream venue), so the documented flow is
client driven.

## Wire format

A FIX 4.4 message is a sequence of `tag=value` fields separated by SOH
(`\x01`). The first three fields are always `8=FIX.4.4`, `9=<bodylen>`,
`35=<msgtype>`. The last field is always `10=<checksum>` where the
checksum is the sum of every byte from `8=` up to and including the SOH
before `10=`, modulo 256, formatted as a three-digit zero-padded decimal.

The parser accepts SOH (`\x01`) on the wire and `|` in tests; pick the
delimiter at construction. Internally the byte set is identical; the test
delimiter is only there so failing assertions print readable strings.

## Tags handled

| Tag | Name | Notes |
|----:|---|---|
| 8 | BeginString | Must be `FIX.4.4`; other versions disconnect. |
| 9 | BodyLength | Numeric, used to find the checksum field. |
| 10 | CheckSum | Computed and verified per message. |
| 34 | MsgSeqNum | In/out tracked; gap detection lives in `Session::handle_message`. |
| 35 | MsgType | See the message table below. |
| 49 | SenderCompID | Peer's CompID. Adopted as TargetCompID on first Logon. |
| 56 | TargetCompID | Must equal our SenderCompID or the session disconnects. |
| 52 | SendingTime | Echoed on outbound; not validated on inbound. |
| 108 | HeartBtInt | From Logon; the transport uses it for timers. |
| 112 | TestReqID | Echoed back on TestRequest. |
| 45 | RefSeqNum | Used in Reject. |
| 58 | Text | Free text on Reject and Logout. |
| 7, 16 | BeginSeqNo, EndSeqNo | ResendRequest range. |
| 11 | ClOrdID | Order entry identifier. |
| 41 | OrigClOrdID | Used by Cancel and CancelReplace. |
| 38, 44, 54, 40, 55, 60 | Order qty, price, side, type, symbol, transact time | Application fields. |
| 6, 14, 17, 31, 32, 37, 39, 150, 151 | AvgPx, CumQty, ExecID, LastPx, LastQty, OrderID, OrdStatus, ExecType, LeavesQty | ExecutionReport fields. |

Tags outside this table are accepted by the parser and ignored by the
session; the FIX spec calls this conformance level "ignore unknown
optional fields".

## Messages handled

| MsgType | Name | Direction | Behavior |
|--------:|---|---|---|
| A | Logon | inbound | Establishes session, resets seq nums if `reset_on_logon=true`. |
| 0 | Heartbeat | both | No-op on inbound; transport sends on schedule. |
| 1 | TestRequest | inbound | Replied with Heartbeat echoing TestReqID. |
| 2 | ResendRequest | inbound | Replied with Logout(text=not supported); see "Out of scope". |
| 3 | Reject | inbound | Logged, no state change. |
| 4 | SequenceReset | inbound | Accepted, no state change (we do not retain history). |
| 5 | Logout | both | Bilateral; second peer to send transitions to Disconnected. |
| 8 | ExecutionReport | outbound | Built by the matching engine, sent through `Session::send_execution_report`. |
| D | NewOrderSingle | inbound | Dispatched as an application message to the matcher. |
| F | OrderCancelRequest | inbound | Dispatched; matcher emits CancelAck or OrdReject. |
| G | OrderCancelReplaceRequest | inbound | Dispatched; matcher handles via cancel + new. |

## State machine

```
   Disconnected
        | (Logon received)
        v
    LoggedIn  <----+
        |          | (TestRequest -> Heartbeat reply)
        | (Logout) |
        v          |
    LogoutSent ----+
        |
        | (peer Logout or timeout)
        v
   Disconnected
```

The active-side `LogonSent` state exists for symmetry but is not used by
the bundled TCP acceptor; it is reachable from `initiate_logon()` and is
covered by unit tests for completeness.

## Sequence number tracking

Each message carries a strictly increasing `MsgSeqNum` (tag 34). On
inbound:

* `seq == in_seq_ + 1`: accept and process.
* `seq > in_seq_ + 1`: send a ResendRequest from `expected` to `0`
  (`0` = open ended in FIX 4.4) and drop the message. We do not advance
  `in_seq_`; the peer is expected to retransmit.
* `seq < in_seq_ + 1`: send Logout and disconnect. The PossDupFlag
  recovery path is intentionally not implemented (see "Out of scope").

Outbound messages increment `out_seq_` exactly once per call to
`build_message`, with the exception of `send_execution_report` which
shares the same builder.

## Out of scope

* Resend history retention. We reply to ResendRequest with Logout. This
  is a cold-start engine; persistent message stores belong in front of a
  recovery layer like `FIXT.1.1` or a SBE replay tap.
* SSL/TLS. Wire `stunnel` or terminate TLS at a sidecar.
* DropCopy, market data sessions (35=W, 35=X), QuoteRequest, Quote.
* PossDupFlag retransmission decoding.

## Timer semantics

`HeartBtInt` is propagated to the transport but the bundled
`net::TcpServer` does not yet implement the on-timer Heartbeat/TestRequest
sender; the session itself is timer-agnostic. The transport layer can be
extended by wiring a `std::thread` per session that calls
`send_heartbeat()` every `HeartBtInt` seconds and disconnects if more than
2.5x `HeartBtInt` elapses without inbound traffic. The bench harness
disables heartbeats by using short-lived sessions.
