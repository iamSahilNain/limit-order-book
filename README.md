# Limit Order Book & Matching Engine (C++)

A single-threaded, in-memory limit order book with a price–time-priority matching
engine, plus a benchmark harness that measures throughput and per-operation
latency percentiles. Built to compare two price-ladder designs: a `std::map`
baseline against a cache-friendly flat array.


## Design

- **Integer tick prices.** Prices are `int64` ticks, not floats — exact equality
  and ordering, cheap comparisons, and the natural index for the array ladder.
- **Price–time priority.** Best price matches first; within a price level, oldest
  order fills first (FIFO).
- **Two ladders benchmarked behind one interface (`IOrderBook`):**
  - `MapOrderBook` — `std::map` keyed by price, `std::list` FIFO per level, and an
    `id → location` index for O(1) cancel via list splice.
  - `ArrayOrderBook` — flat array indexed by `(price − base) / tick`, with
    best-bid/ask cursors. *(Checkpoint 1.)*
- **No hot-path allocation.** `add()` appends trades to a caller-owned buffer that
  is reused across calls, so a warm run allocates nothing per order.

### Complexity (MapOrderBook)

| Operation           | Cost            |
|---------------------|-----------------|
| `best_bid`/`best_ask` | O(1)          |
| `add` (rests)       | O(log L)        |
| `add` (matches)     | O(log L + F)    |
| `cancel`            | O(log L)        |

`L` = distinct price levels on a side, `F` = resting orders consumed by a match.

## Layout

```
include/lob/   types, IOrderBook interface, MapOrderBook, ArrayOrderBook, memory_pool
src/           MapOrderBook, ArrayOrderBook (fully implemented)
bench/         order generator + benchmark harness (both books)
tests/         8 unit tests × 2 books + differential fuzz test (200k ops)
```

## Build & run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure   # correctness
./build/bench 2000000                         # throughput + latency
```

No cmake? Compile directly:

```bash
g++ -std=c++20 -O3 -march=native -Iinclude \
    src/map_order_book.cpp src/array_order_book.cpp tests/test_order_book.cpp -o tests && ./tests
g++ -std=c++20 -O3 -march=native -Iinclude -I. \
    src/map_order_book.cpp src/array_order_book.cpp bench/benchmark.cpp -o bench && ./bench
```

## Checkpoints

These are intentionally left for you to implement — they are the parts an
interviewer will probe, and the source of your published numbers.

1. **Flat-array price ladder** (`array_order_book.*`). Map price → array slot,
   maintain best-bid/ask cursors, reuse the existing tests, then uncomment
   `ArrayOrderBook` in `benchmark.cpp` and A/B it against the map.
2. **Memory pool + intrusive list** (`memory_pool.hpp`). Preallocated node arena
   with an intrusive free list; back the array ladder's levels with it so cancel
   is an O(1) unlink and the hot path never calls the allocator. Measure add/cancel
   latency before vs. after.
3. **Publish real numbers.** Run the benchmark on your machine, record the results
   in the table below, and make the resume match this table.

## Benchmark results

Measured on Apple M1 Pro, 2M ops (75% Add / 25% Cancel), `g++ -std=c++20 -O3 -march=native`.
p50/p90 show 0 ns because individual ops are below `std::chrono::steady_clock` resolution
(~1 µs tick on macOS); mean and throughput are the reliable metrics here.

| Ladder          | Throughput (ops/s) | p50 (ns) | p99 (ns) | mean (ns) |
|-----------------|--------------------|----------|----------|-----------|
| MapOrderBook    | 9,835,259          | 0        | 1,000    | 83        |
| ArrayOrderBook  | 13,701,352         | 0        | 1,000    | 55        |

ArrayOrderBook is **~1.4× faster** (39% lower mean latency) due to contiguous memory,
no tree rebalancing, integer-indexed slot access, and zero allocator calls on the hot path.

## Notes for defending this project

- Throughput and latency are two views of one number: `throughput ≈ 1 / mean
  latency`. If they don't reconcile, one measurement is wrong.
- `std::chrono` sampling costs ~20–40 ns per call — non-trivial for sub-100 ns ops.
  Know when to switch to `rdtsc` or batch timing.
- The `IOrderBook` interface is virtual for pluggability/benchmarking; production
  would template/CRTP to drop the vtable from the hot path. Know the trade-off.
- Be ready to explain: why integer ticks, why FIFO within a level, why cancel is
  O(1) on the list but O(log L) overall on the map, and exactly why the array
  ladder wins (contiguity, no allocation, no rebalancing) and where it loses
  (bounded band, out-of-band prices).
