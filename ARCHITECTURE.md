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

## Sequence number recovery (out of scope)

The session sends ResendRequest on gap detection per spec, but replies to
incoming ResendRequest with Logout because this engine does not retain
message history. Production fronting typically pairs the engine with a
recovery tap (FIXT.1.1 store, SBE replay, or a database-backed
EventStore) that can replay by sequence. That tap is not part of this
repo.

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
