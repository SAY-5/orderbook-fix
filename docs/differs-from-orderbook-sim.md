# How orderbook-fix differs from orderbook-sim

`SAY-5/orderbook-sim` and `SAY-5/orderbook-fix` are both C++20 matching
engines but they study different layers of the same problem.

| Axis | orderbook-sim | orderbook-fix |
|---|---|---|
| Wire protocol | Custom ASCII (`B 100 1000`) | FIX 4.4 over TCP, full session |
| Matching algorithm | FIFO (price-time priority) | Pro-rata default, FIFO switchable |
| Concurrency | SPSC ring, single-thread matcher | One thread per FIX session, shared matcher |
| What it stresses | Lock-free queueing, branch-prediction | Wire parsing, session state, allocation math |
| Failure modes covered | Empty/full ring, single-producer overflow | Checksum bad, gap detection, malformed framing, peer logout |
| What it skips | FIX, network, sessions | The lock-free ring and the SPSC contract |

## What you learn from each repo

`orderbook-sim` is the queueing and matching primitives in isolation. The
focus there is the data layout (intrusive list price levels, vector of
levels), the lock-free SPSC ring as a producer-consumer interface, and
the cost of branchy hot paths.

`orderbook-fix` is the protocol and allocation layer. The hot path looks
small from the matching side because the wire parsing, checksum, session
state, and per-level pro-rata math dominate. The bench numbers
(~2.6k msgs/sec pro-rata, ~6.8k msgs/sec FIFO on a busy book) show that
the algorithm choice is the load-bearing decision, not the I/O.

## Why duplicate the order book

The two repos share *no* code. The order book and price level types in
`src/core/` are fresh implementations, not imports. This is deliberate:

1. Each repo should compile and test standalone.
2. The matching algorithms have different invariants: FIFO walks
   head-to-tail and stops, pro-rata snapshots the level. Sharing a
   single base class would have leaked one algorithm's invariants into
   the other.
3. The intrusive-list pattern is short enough (~50 lines) that
   duplication is cheaper than a shared library.

If a future repo needs both, the cleanest extraction point is
`PriceLevel` plus an `Allocator` strategy interface.
