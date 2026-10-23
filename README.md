# Harbor

A low-latency exchange matching engine in C++20.

Harbor matches buy and sell orders the way a stock exchange does: best price
first, then first come, first served. It ships two order books with identical
behavior: a simple one that is easy to trust, and a fast one built for latency.
A randomized differential test runs a million operations through both and
requires them to agree after every single one.

## Status

| Version | Scope | State |
|---|---|---|
| **v1** | Order book, price-time matching, IOC, cancel, replace, tests, benchmarks | In progress: books and tests done, benchmarks next |
| v2 | Write-ahead log, snapshots, crash recovery | Planned |
| v3 | TCP order gateway, UDP market-data feed | Planned |
| v4 | Hot standby that replays the command log, with failover | Planned |

## How matching works

```
Sellers (asks)              Buyers (bids)
  sell 100 @ 151              buy 50 @ 149
  sell 200 @ 152              buy 80 @ 148

Incoming: buy 150 @ 152
  -> fills 100 @ 151 (cheapest seller first)
  -> fills  50 @ 152
  -> 150 left resting at 152 on the sell side
```

Rules:

- **Price-time priority.** Best price first; at the same price, earliest order first.
- **Trade price** is always the resting order's price.
- **GTC** orders rest in the book with whatever is left after matching.
  **IOC** (immediate-or-cancel) orders fill what they can and discard the rest.
- **Replace** keeps the order's place in line only when it reduces quantity at
  the same price. Raising the quantity or changing the price sends it to the
  back of the queue, and the new price can trade immediately.
- **Rejects** are returned as values (`BadPrice`, `BadQuantity`, `DuplicateId`,
  `UnknownOrder`, `BookFull`), never thrown. A rejected call changes nothing.

Prices are integers in ticks within a configured range, so no floating-point
rounding ever touches matching.

## Design

Both books expose the same API (`add`, `cancel`, `replace`, `bestBid`,
`bestAsk`, `depthAt`) and are tested with the same suite.

### ReferenceBook: the oracle

`std::map<Price, std::list<Order>>` for each side plus an
`std::unordered_map` from order id to its list position. About 200 lines and
obviously correct, which is the point: when the two books disagree, this one is
presumed right.

### FastBook: the fast path

| Structure | Purpose |
|---|---|
| Array of price levels indexed by `(price - min) / tick` | O(1) level lookup, no tree |
| Occupancy bitset, one bit per level | Finds the next best price with `countr_zero` / `countl_zero` |
| Intrusive doubly linked FIFO per level (slot indices, not pointers) | O(1) append and O(1) removal from the middle on cancel |
| Fixed slot pool with a free list | No `new` / `malloc` after construction |
| `FlatHashMap` (open addressing, linear probing, backward-shift deletion) | O(1) order-id lookup without per-node allocation |

| Operation | Cost |
|---|---|
| Rest an order | O(1) |
| Cancel | O(1) typical |
| Match | O(levels touched + fills) |
| Find new best after a level empties | Scans the bitset outward from the old best, so usually one word |

When a level empties, the next best price is searched for starting from the
old best, not from the start of the bitset; on a 1,000,000-tick range that is
the difference between one word and up to 15,625.

When the book is full, an incoming GTC order is rejected only if it does not
cross the spread. An order that crosses either fills completely or fully
consumes at least one resting order, which frees the slot it needs.

## Testing

- **Behavior suite** (24 cases, run on both books via GoogleTest typed tests):
  priority, partial fills, multi-level sweeps, IOC, cancel, every replace rule,
  every reject, and edge cases such as prices at both ends of the range.
- **Invariants** checked after every operation: the book is never crossed,
  level totals equal the sum of their orders, a bitset bit is set exactly when
  its level is non-empty, the cached best price matches a full scan, and the
  index matches the live orders.
- **Differential test**: a seeded generator issues 1,000,000 adds, cancels, and
  replaces against both books and compares rejects, fills, best prices, and
  order counts after every operation, plus full depth every 1,000. Cancel and
  replace targets are drawn from orders that are actually resting, so the run
  exercises roughly 270K live cancels, 89K live replaces, 120K fills, and 72K
  `BookFull` rejects. A deliberately planted bug in FastBook's replace logic is
  caught within the first 1,000 operations.
- **Sanitizers**: the whole suite also runs under AddressSanitizer and
  UndefinedBehaviorSanitizer.
- **CI**: GCC and Clang on Linux, each with and without sanitizers.

Replay a differential failure with the seed it prints:

```bash
HARBOR_SEED=12345 ./build/harbor_tests --gtest_filter='Differential.*'
```

## Build and test

Requires CMake 3.24+ and a C++20 compiler. GoogleTest is fetched automatically.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

With sanitizers:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DHARBOR_SANITIZE=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

## Layout

```
include/harbor/   types, FlatHashMap, ReferenceBook, FastBook
src/              book implementations
tests/            behavior suite, hash map tests, differential test, op generator
docs/             design spec
```
