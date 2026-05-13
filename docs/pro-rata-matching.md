# Pro-rata matching with FIFO residual tiebreaker

## Algorithm

Given an aggressive order of quantity `A` that crosses a price level
holding `n` resting orders with leaves quantities `q_1, q_2, ..., q_n` in
time order (`q_1` is the oldest), pro-rata allocation produces fills
`f_1, ..., f_n` such that:

```
T = sum(q_i)
to_fill = min(A, T)
f_i_initial = floor(to_fill * q_i / T)
residual = to_fill - sum(f_i_initial)
```

The residual (always `< n`, because each floor loses at most 1 unit and
there are `n` floors) is distributed in time order across orders that
still have capacity (`q_i - f_i_initial > 0`):

```
for i in 1..n:
    if residual == 0: break
    cap_i = q_i - f_i_initial
    take = min(cap_i, residual)
    f_i_initial += take
    residual -= take
```

The aggressor's remaining quantity after this level is
`A - sum(f_i_final)`. If positive, it walks to the next price level (in
strict price priority for the aggressor's side).

## Snapshot invariant

A subtle point in the implementation (`src/core/matcher.cpp::fill_level_prorata`):
the denominator `T` must be the snapshot taken at function entry. If the
denominator were recomputed after each individual fill, an order partly
filled earlier in the same pass would shrink `T`, and the next order would
absorb more than its fair share. The reference algorithm is the snapshot
version; the implementation matches it.

## Worked examples

### Equal sizes

Aggressor 300, three resting at the same price size 100 each.

```
T = 300, to_fill = 300
f_i = floor(300 * 100 / 300) = 100 each
residual = 0
fills: 100, 100, 100
```

### The README example: 300/300/400

Aggressor 1000, three resting at $100 size 300, 300, 400.

```
T = 1000, to_fill = 1000
f_1 = floor(1000 * 300 / 1000) = 300
f_2 = floor(1000 * 300 / 1000) = 300
f_3 = floor(1000 * 400 / 1000) = 400
residual = 0
fills: 300, 300, 400
```

### Rounding residual (the case the tiebreaker matters)

Aggressor 500, three resting at the same price size 100, 200, 300.

```
T = 600, to_fill = 500
f_1 = floor(500 * 100 / 600) = 83
f_2 = floor(500 * 200 / 600) = 166
f_3 = floor(500 * 300 / 600) = 250
sum = 499, residual = 1
FIFO assigns the 1 unit to order 1 (oldest).
fills: 84, 166, 250
```

This is the exact case the `ProRata.ProportionalAllocationWithRounding`
unit test pins.

### Pathological residual

5 orders of size 7 at the same price, aggressor 11.

```
T = 35, to_fill = 11
f_i = floor(11 * 7 / 35) = 2 each
sum = 10, residual = 1
FIFO assigns the 1 unit to order 1.
fills: 3, 2, 2, 2, 2
```

## Why FIFO for the residual

Two production conventions exist for the residual:

1. **FIFO** (what this engine does): the oldest order with remaining
   capacity gets the leftover units. Preserves price-time priority for
   the tiebreaker, which is what most CME and ICE pro-rata products use.
2. **Largest order**: the order with the largest leaves quantity at entry
   gets the leftover. Used by some agricultural futures because it
   reduces order-splitting incentives.

The largest-order variant is not implemented; switching it on would be a
small change in `fill_level_prorata` (sort `slots` by `orig_leaves` after
step 2 and walk that order).

## Correctness invariants

The unit test `pro_rata_test.cpp` covers:

* Equal sizes split evenly.
* The README spec example (300/300/400 -> 300/300/400).
* Proportional allocation with rounding residual (the 100/200/300 case).
* Aggressor larger than level fills the level and rests the residual.
* Single-order level gets everything.
* Residual uses FIFO tiebreaker (5x7 vs 11 case).
* Walks across multiple price levels in strict price priority.
* Fully-filled orders are removed from the book index.

## Comparison with FIFO matching

FIFO walks the level head-to-tail and fills each order in turn until the
aggressor is exhausted. It is simpler and faster (it stops as soon as the
aggressor is full), but it concentrates fills on the oldest order, which
discourages quoting size at the front of the queue. Pro-rata spreads
fills across the level proportionally, encouraging size at the expense of
order-of-arrival priority.

Most CME interest-rate futures use pro-rata. Most equity venues use FIFO.

You can switch the engine algorithm at runtime via `Matcher::set_algo`
or, for the server binary, the `MATCH_ALGO` env var (`prorata` or `fifo`).
