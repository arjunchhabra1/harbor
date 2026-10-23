# Harbor

[![CI](https://github.com/arjunchhabra1/harbor/actions/workflows/ci.yml/badge.svg)](https://github.com/arjunchhabra1/harbor/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A low-latency exchange matching engine in C++20.

| Version | Scope | Status |
|---|---|---|
| v1 | Price-time order book: limit, IOC, cancel, replace | Done |
| v2 | Write-ahead log, snapshots, crash recovery | In progress |
| v3 | TCP order gateway, UDP market data | Planned |
| v4 | Hot standby with failover | Planned |

## Design

- **FastBook**: price-level array, occupancy bitset (`ctz`/`clz` to find the
  next best price), intrusive FIFO per level, fixed slot pool, open-addressing
  id index. No heap allocation after construction.
- **ReferenceBook**: `std::map` + `std::list`, kept simple as a correctness oracle.
- Matching: best price first, then time; trades at the resting price; replace
  keeps queue position only when reducing quantity at the same price.

## Performance

Apple M3, Apple clang 16, `-O2 -march=native`, 10M generated operations
([method](bench/RESULTS.md)):

| Book | Throughput | ≈ ns/op | Heap allocations |
|---|---|---|---|
| FastBook | 26.9M ops/s | 37 | 0 |
| ReferenceBook | 12.4M ops/s | 81 | 8.8M |

## Testing

- A shared behavior suite runs on both books, with invariants checked after
  every operation.
- A seeded differential test drives both books with 1M+ operations and requires
  identical fills, rejects, and depth after each one, including at the edges of
  the price range.
- Everything runs under ASan and UBSan in CI (GCC and Clang).

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Add `-DHARBOR_SANITIZE=ON` for sanitizers. Benchmarks: configure with
`-DCMAKE_BUILD_TYPE=Release` and run `./build-rel/harbor_bench`.

MIT licensed.
