# orderbook-fix

C++20 matching engine that accepts orders over a FIX 4.4 session, applies
pro-rata allocation at each price level, and supports the standard order
lifecycle (New, Cancel, Replace) with full FIX session semantics.

The repo is a portfolio project. It is small enough to read end-to-end in
one sitting and large enough to make the algorithm and protocol choices
visible.

## What this studies

Two things that production matching engines care about and most
tutorials skip:

1. **FIX 4.4 session semantics.** Pipe-delimited tag-value wire format,
   3-digit checksum, sequence numbers, gap detection, heartbeats,
   bilateral logout. The session is a pure state machine
   (`fix::Session::on_bytes` is total over its input). The TCP transport
   is a thin layer above it.
2. **Pro-rata allocation at each price level.** Orders at the same price
   share fills proportionally to their size, with FIFO tie-breaking for
   the rounding residual. FIFO matching is the simpler alternative and
   is included as a runtime-switchable mode for direct comparison.

## How this differs from `SAY-5/orderbook-sim`

| Axis | orderbook-sim | orderbook-fix |
|---|---|---|
| Wire protocol | Custom ASCII (`B 100 1000`) | FIX 4.4 over TCP, full session |
| Matching algorithm | FIFO (price-time priority) | Pro-rata default, FIFO switchable |
| Concurrency | SPSC ring, single-thread matcher | One thread per FIX session, shared matcher under coarse lock |
| What it stresses | Lock-free queueing, branch prediction | Wire parsing, session state, allocation math |

`orderbook-fix` is the protocol + algorithm layer. `orderbook-sim` is the
queueing and matching primitives in isolation. The two repos share no
code; see `docs/differs-from-orderbook-sim.md` for why.

## Bench numbers

End-to-end FIX session bench, measured on:

* Apple M2 Pro, macOS 26.0 (Darwin 25.0), Apple Clang 17.0
* Release build (`-O3`), loopback TCP, single accepting thread

The workload is 100k (50k for pro-rata) `NewOrderSingle` messages
alternating buy/sell around a 10-tick spread. Latency is wall-clock from
"client wrote FIX bytes" to "client received first ExecutionReport
bytes" through the kernel loopback. Warmup of 2000 messages is excluded.

Pro-rata, 50k messages on a continuously deepening book:

```json
{
  "n_messages": 50000,
  "algo": "prorata",
  "wall_seconds": 18.929877,
  "msgs_per_sec": 2641.33,
  "avg_ns": 350488,
  "p50_ns": 524288,
  "p95_ns": 1048576,
  "p99_ns": 4194304,
  "p999_ns": 4194304
}
```

FIFO, 50k messages, same workload:

```json
{
  "n_messages": 50000,
  "algo": "fifo",
  "wall_seconds": 7.354462,
  "msgs_per_sec": 6798.59,
  "avg_ns": 121684,
  "p50_ns": 65536,
  "p95_ns": 524288,
  "p99_ns": 1048576,
  "p999_ns": 1048576
}
```

Pro-rata pays ~2.5x in throughput because it snapshots every resting
order at the touched level (O(level depth) per match), while FIFO stops
as soon as the aggressor is filled. The latency histogram is logarithmic
(factor-2 buckets); the P99 numbers reflect the deepening book over the
run, not jitter. Raw outputs live in `bench/results/`.

To reproduce: `make bench` (CMake + Release build, no extra deps).

## Architecture

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

Full architecture, sequence-number recovery semantics, and concurrency
notes are in `ARCHITECTURE.md`.

## Pro-rata worked example

Aggressor: buy 1000 at $100.
Resting at $100, in time order: A=300, B=300, C=400.

```
T = 1000, to_fill = 1000
A: floor(1000 * 300 / 1000) = 300
B: floor(1000 * 300 / 1000) = 300
C: floor(1000 * 400 / 1000) = 400
residual = 0
Fills: A=300, B=300, C=400.
```

When rounding leaves a residual (aggressor 500 vs A=100, B=200, C=300):

```
T = 600, to_fill = 500
A: floor(500 * 100 / 600) = 83
B: floor(500 * 200 / 600) = 166
C: floor(500 * 300 / 600) = 250
sum = 499, residual = 1 -> goes FIFO to A (oldest)
Fills: A=84, B=166, C=250.
```

The pinned unit test for this case is `pro_rata_test.cpp::ProportionalAllocationWithRounding`.

## Build

```
make build      # Release
make test       # ctest
make bench      # writes bench/results/bench_local.json
make asan       # ASan + UBSan
make tsan       # ThreadSanitizer
make fmt-check  # clang-format
```

CMake target: 3.20+. C++20. The only external dependency is GoogleTest,
fetched at configure time via `FetchContent`. libFuzzer is opt-in via
`-DOBFIX_FUZZ=ON` and requires clang.

## Quick start

```
make build
./build/obfix_server --port 9876 --algo prorata &
# now connect with a FIX client to 127.0.0.1:9876
```

Or via docker-compose:

```
docker compose up --build
```

## What this is not

* Not FIX 5.0. We speak 4.4 only.
* Not FIXT.1.1. The transport is plain TCP; there is no separate transport
  session.
* Not a market data session. No QuoteRequest (35=R), Quote (35=S), or
  market data subscription (35=V/W/X). Order entry only. For a market
  data side, see (future) `SAY-5/mdfeed-itch`.
* Not a DropCopy publisher.
* No pre-trade risk checks. No STP, no cross-prevention.
* No persistent message store. Cold-start only; ResendRequest from the
  peer is answered with Logout. Production deployments pair this with a
  recovery tap.
* No SSL/TLS on FIX. Production wires `stunnel` or terminates TLS at a
  sidecar.

## Tests

7 test binaries, 80 individual test cases. Coverage in numbers:

* `fix_parser_test`: 35 cases (valid messages, malformed, checksum bad,
  truncated, duplicate tag, large body, round-trip fidelity).
* `fix_session_test`: 15 cases (state transitions, gap detection,
  TestRequest, peer Logout, malformed framing).
* `pro_rata_test`: 8 cases (the math correctness lives here).
* `matcher_test`: 7 cases (FIFO vs pro-rata equivalence on degenerate
  inputs, hot-swap, walk across levels).
* `order_book_test`, `price_level_test`: 14 combined.
* `session_e2e_test`: 1 case driving a real TCP loopback session.

CI runs the suite under GCC and Clang, then again under ASan+UBSan,
TSan, and the libFuzzer smoke. See `.github/workflows/ci.yml`.

## License

MIT. See `LICENSE`.
