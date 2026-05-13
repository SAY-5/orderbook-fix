# Architecture

## Layers

```
+--------------------------------------------------+
|  net::TcpServer    (one thread per session)      |
|    |                                             |
|    | recv() bytes                                |
|    v                                             |
|  fix::Session      (state machine, seq tracking) |
|    |                                             |
|    | AppMessage (NewOrderSingle / Cancel / G)    |
|    v                                             |
|  core::Matcher     (pro-rata or FIFO dispatch)   |
|    |                                             |
|    | OrderBook + PriceLevel mutation             |
|    v                                             |
|  core::OrderBook   (per-symbol, intrusive list)  |
+--------------------------------------------------+
|  ExecutionReport bytes <- fix::Session <- events |
+--------------------------------------------------+
```

The session layer is pure (`on_bytes -> SessionStep`). The transport
layer (TcpServer) is the only place that does I/O. The matcher operates
under a coarse lock on the shared book; sessions feed it serially.

## FIX session state machine

```
                +---------------+
                | Disconnected  |
                +---------------+
                  |          ^
       initiate   |          | peer Logout or
       _logon()   |          | bad framing
                  v          |
                +---------------+
                |  LogonSent    |  (active side; bundled
                +---------------+   acceptor does not use)
                  | peer Logon  ^
                  v             |
                +---------------+
                |  LoggedIn     |---> matcher dispatch
                +---------------+
                  | send_logout |
                  v             ^
                +---------------+
                | LogoutSent    | peer Logout -> Disconnected
                +---------------+
```

Gap detection (`MsgSeqNum > expected`) emits a ResendRequest and drops
the message. Lower-than-expected emits Logout and disconnects. The
PossDupFlag recovery path is intentionally not implemented.

## Pro-rata allocator

The allocator walks one price level at a time. At each level:

1. Snapshot every resting order's `leaves_qty` into `slots` (vector).
   This is the denominator `T`.
2. For each slot, compute `floor(to_fill * size_i / T)`.
3. Distribute the rounding residual (`< n`) FIFO across slots that have
   remaining capacity.
4. Emit one Trade per slot with non-zero allocation, decrement
   `leaves_qty`.

After matching, the caller (`Matcher::submit`) sweeps each touched
level and removes any order with `leaves_qty == 0` from the book's
`session, ClOrdID` index. The sweep is separate from the fill so the
allocator can run unaware of the index.

See `docs/pro-rata-matching.md` for the worked examples and the rationale
for the FIFO-residual tiebreaker.

## Sequence number recovery

Gap detection (inbound seq > expected) emits a ResendRequest on the
outbound side.

Inbound ResendRequest is fulfilled from a bounded outbound history ring.
Each message the session sends with a session-assigned MsgSeqNum is
retained as `(seq, original_sending_time, wire_bytes)` until the ring's
configured cap (`SessionConfig::outbound_history_size`, default 10000) is
exceeded, at which point the oldest entry is dropped.

When a ResendRequest arrives for `[BeginSeqNo, EndSeqNo]` (with EndSeqNo
= 0 meaning "to current end"):

* Seqs that fell out of the ring are coalesced into a single
  SequenceReset-GapFill (35=4, GapFillFlag=Y, NewSeqNo=oldest-retained).
  The GapFill carries the seq of `BeginSeqNo` so the peer applies it as
  filling the gap.
* Retained seqs are replayed by re-parsing the stored bytes, splicing in
  `PossDupFlag=Y` (43) and `OrigSendingTime=<original SendingTime>` (122),
  and re-serializing. The retained `SendingTime` is overwritten with
  "now" per FIX 4.4 rules.
* Seqs the peer asked for that are outside the session's known range are
  ignored (no NACK; this matches what most accepting venues do).

`SessionConfig::outbound_history_size = 0` disables retention; in that
mode every resend collapses to a single GapFill bumping the peer past
the unknown range. That config is useful for memory-pinned deployments
where the venue is expected to handle recovery itself.

The PossDupFlag *inbound* path (the peer's own replays) is still out of
scope: the session does not parse `PossDupFlag=Y` to suppress
double-processing of app messages. Adding that is straightforward but
requires per-message dedup at the matcher layer, which is left as future
work.

## Concurrency model

* One acceptor thread on the listen socket.
* One session thread per accepted client. Each thread owns its
  `fix::Session` and its socket. No shared state inside a session.
* A single `core::OrderBook` and `core::Matcher` instance is shared
  across sessions, protected by `book_mu_` (a `std::mutex`). The lock is
  held only while routing an inbound application message through the
  matcher.
* Atomic counters track total messages in/out for the bench harness.

The shared-matcher-under-mutex design is intentional for portfolio
clarity: a sharded-matcher option (one book per symbol, one thread per
shard) is a known next step and is documented as future work. The
ThreadSanitizer CI job catches any race in the session code itself.

## File layout

```
src/
  core/        order book, matcher, pro-rata allocator, types
  fix/         wire parser, serializer, session state machine
  net/         TCP server (one thread per session)
  obs/         histogram and logger
  main.cpp     server entry point
tests/
  unit/        per-component GoogleTest binaries
  fuzz/        libFuzzer harness for the wire parser
  integration/ end-to-end TCP loopback test
bench/         replay harness + committed bench_local.json
docs/          this and other deep dives
```
