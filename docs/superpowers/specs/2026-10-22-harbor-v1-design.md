# Harbor v1: order book and matching

Date: 2026-10-22
Status: design approved in conversation, awaiting written-spec review

## Purpose

Harbor is a low-latency exchange matching engine in C++20, built as a
portfolio project for software engineering roles. The author writes the
implementation; every design decision must be explainable in an interview.

Harbor grows in versions, each a complete project on its own:

| Version | Adds |
|---|---|
| **v1 (this spec)** | Order book, price-time matching, IOC, cancel, replace; tests; benchmarks |
| v2 | Write-ahead log, snapshots, crash recovery |
| v3 | TCP order gateway, UDP market-data feed |
| v4 | Hot standby replicating the command log, with failover |

### v1 success criteria

1. Both books pass the shared behavior tests and invariant checks.
2. The randomized differential test runs 1M+ operations with zero mismatches
   between `ReferenceBook` and `FastBook`.
3. ASan and UBSan builds run all tests clean.
4. Benchmarks report throughput and p50/p99/p99.9/max latency for both books,
   and FastBook performs zero heap allocations after construction.
5. CI builds and tests on Linux with GCC and Clang.

### Out of scope for v1

Networking, persistence, multiple symbols in one process, threads, market
orders, self-trade prevention, FOK.

## Core types

```cpp
namespace harbor {

using Price    = std::int64_t;   // in ticks
using Quantity = std::uint32_t;
using OrderId  = std::uint64_t;
using ClientId = std::uint32_t;

enum class Side { Buy, Sell };
enum class TimeInForce { GTC, IOC };

struct NewOrder {
    OrderId id; ClientId client; Side side; Price price; Quantity qty; TimeInForce tif;
};

struct Fill { OrderId aggressor; OrderId resting; Price price; Quantity qty; };

enum class Reject { None, DuplicateId, UnknownOrder, BadPrice, BadQuantity, BookFull };

}  // namespace harbor
```

## Book interface

`ReferenceBook` and `FastBook` expose the same member functions. They are not
related by inheritance (no virtual calls on the hot path); tests use GoogleTest
typed tests to run the same cases on both.

```cpp
Reject add(const NewOrder& order, std::vector<Fill>& out);
Reject cancel(OrderId id);
Reject replace(OrderId id, Price newPrice, Quantity newQty, std::vector<Fill>& out);

std::optional<Price> bestBid() const;
std::optional<Price> bestAsk() const;
std::uint64_t depthAt(Side side, Price price) const;  // 0 for empty or invalid prices
std::size_t orderCount() const;
bool checkInvariants(std::string& error) const;
```

`out` is cleared by the callee on entry and filled with the trades the call
produced, in execution order. Callers reuse one vector across calls.

## Matching rules

- **Price-time priority.** Bids match highest price first, asks lowest price
  first. Within a price level, earliest arrival first.
- **Crossing.** A buy crosses while `buy.price >= bestAsk`; a sell crosses while
  `sell.price <= bestBid`.
- **Trade price** is always the resting order's price.
- **Partial fills.** A partially filled resting order keeps its queue position.
  A GTC aggressor with quantity left after crossing rests at its limit price at
  the back of that level.
- **IOC.** The unfilled remainder is discarded; an IOC order never rests.
- **Same-client orders may match** each other in v1 (no self-trade prevention).

### Validation (checked before any state change, in this order)

1. `qty == 0` → `BadQuantity`.
2. Price off-tick, or outside the book's `[minPrice, maxPrice]` → `BadPrice`.
   `ReferenceBook` is constructed with the same range and tick so both books
   reject identically.
3. `id` belongs to a live (resting) order → `DuplicateId`. IDs of filled or
   cancelled orders may be reused.
4. The order would need to rest and the order pool is full → `BookFull`
   (`ReferenceBook` takes the same `maxOrders` limit so both books agree).
   An order that fills completely never needs a slot.

A rejected call produces no fills and changes nothing.

### Cancel

Removes a resting order. Unknown, filled, or already-cancelled id →
`UnknownOrder`.

### Replace

Applies only to a resting order; otherwise `UnknownOrder`. The order keeps its
id, side, client, and TIF (always GTC, since IOC orders never rest).
`newQty` is the new remaining quantity.

- `newQty == 0` → `BadQuantity` (use cancel). Price validated as for `add`.
- Same price and `newQty <= remaining`: quantity updated in place, **queue
  position kept**. (`newQty == remaining` is a successful no-op.)
- Price changed or `newQty > remaining`: the order is removed and re-entered as
  a new arrival with the new price and quantity — **queue position lost** — and
  may match immediately. Fills from that match report the replaced order's id
  as `aggressor`.

## ReferenceBook

Optimized for obviousness, not speed.

```cpp
std::map<Price, std::list<Order>, std::greater<>> bids_;   // best = begin()
std::map<Price, std::list<Order>>                 asks_;   // best = begin()
std::unordered_map<OrderId, Locator> index_;  // side, price, list iterator
```

Empty levels are erased from the maps. When the two books disagree, the
reference book is presumed correct until proven otherwise.

## FastBook

```cpp
struct Order {
    OrderId id; ClientId client; Price price; Quantity remaining; Side side;
    std::uint32_t prev, next;              // pool slot indices; kNone = UINT32_MAX
};
struct Level { std::uint32_t head, tail; std::uint64_t total; };

std::vector<Order>         pool_;         // preallocated, maxOrders slots
std::uint32_t              freeHead_;     // free list threaded through Order::next
std::vector<Level>         bidLevels_, askLevels_;  // index = (price - minPrice) / tick
std::vector<std::uint64_t> bidBits_, askBits_;      // bit set ⇔ level non-empty
FlatHashMap<OrderId, std::uint32_t> index_;         // id → pool slot
std::uint32_t              bestBidIdx_, bestAskIdx_; // cached; kNone when empty
std::size_t                count_;
```

Constructor: `FastBook(Price minPrice, Price maxPrice, Price tick, std::uint32_t maxOrders)`.
All memory is allocated in the constructor. Level totals are 64-bit because
many 32-bit order quantities can sum past 2³².

| Operation | Method | Cost |
|---|---|---|
| Rest an order | pop free slot, append at level tail, set bit, update cached best | O(1) |
| Cancel | hash lookup, unlink, clear bit if level empties, recompute best if needed | O(1) typical |
| Match | walk from cached best level, consume from heads | O(levels touched + fills) |
| New best after a level empties | scan bitset words **from the old best outward**, using `ctz`/`clz` | O(1) typical, O(range/64) worst |

Scanning from the old best (not from index 0) is a deliberate improvement over
designs that rescan the whole bitset.

## FlatHashMap

Open addressing with linear probing, power-of-two capacity
(`>= 2 × maxOrders`, so load factor ≤ 0.5), keys hashed with a 64-bit mixer
(splitmix64 finalizer). Deletion uses backward-shift (no tombstones), so probe
sequences stay short under heavy cancel traffic. API: `insert`, `find`,
`erase`, `size`. Fixed capacity; no rehashing.

## Testing

GoogleTest, fetched by CMake `FetchContent`. Tests are written before each
implementation step.

1. **FlatHashMap unit tests**, including erase-in-middle-of-cluster and
   wraparound at the end of the table.
2. **Behavior tests** (typed, run on both books): resting and depth, price
   priority, time priority, partial fills both ways, multi-level sweep, trade
   price, IOC remainder, cancel and cancel errors, replace keeping vs losing
   priority, replace that crosses, every reject code.
3. **Invariants**, checked after every operation in tests: book not crossed;
   each level's total equals the sum of its orders; bitset bit set exactly when
   the level is non-empty; cached best equals a full scan; index size equals
   live order count.
4. **Differential test.** A seeded generator produces 1M operations: ~60% add
   (~20% of them IOC), ~30% cancel, ~10% replace; prices drawn from a window of
   ~200 ticks around a midpoint so many orders cross; ids sometimes reused and
   sometimes invalid to exercise rejects. After every operation: identical
   reject codes and fill lists, identical best bid/ask. Every 1,000 operations:
   identical `depthAt` for every price in the window. On mismatch the test
   prints the seed and operation index. The seed is configurable via an
   environment variable for replaying failures.
5. **Sanitizers.** A `HARBOR_SANITIZE` CMake option builds tests with
   `-fsanitize=address,undefined`.

## Benchmarks

`bench/bench_books.cpp`, Release build, `-O2 -march=native`.

- **Throughput:** 10M pre-generated operations (same mix as the differential
  test) run through each book; report operations per second.
- **Latency:** each operation timed with `std::chrono::steady_clock` over ≥1M
  samples after a 100K-operation warm-up; report p50, p99, p99.9, max.
- **Allocations:** global `operator new` override counts allocations after
  warm-up; expected zero for FastBook.
- Results, with CPU, OS, compiler, and flags, are recorded in
  `bench/RESULTS.md`. No targets are set in advance; the README reports what
  was measured.

## Layout

```
harbor/
  CMakeLists.txt            C++20, -Wall -Wextra -Wpedantic -Werror, HARBOR_SANITIZE
  include/harbor/
    types.hpp
    flat_hash_map.hpp
    reference_book.hpp
    fast_book.hpp
  src/
    reference_book.cpp
    fast_book.cpp
  tests/
    test_flat_hash_map.cpp
    test_book_behavior.cpp
    test_differential.cpp
  bench/
    bench_books.cpp
    RESULTS.md
  docs/superpowers/specs/   this spec
  README.md
  .github/workflows/ci.yml  Linux, GCC + Clang, normal + sanitizer builds
```

## Build order

Each step lands with its tests passing:

1. CMake skeleton, `types.hpp`, GoogleTest wired up, CI.
2. `FlatHashMap` and its tests.
3. `ReferenceBook` and the behavior tests.
4. `FastBook`, passing the same behavior tests and invariants.
5. Differential test.
6. Benchmarks and `RESULTS.md`.
7. README with design notes and measured numbers.
