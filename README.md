# Harbor

[![CI](https://github.com/arjunchhabra1/harbor/actions/workflows/ci.yml/badge.svg)](https://github.com/arjunchhabra1/harbor/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A low-latency exchange matching engine in C++20.

| Version | Scope | Status |
|---|---|---|
| v1 | Price-time order book: limit, IOC, cancel, replace | Done |
| v2 | Write-ahead log, snapshots, crash recovery | Done |
| v3 | TCP order gateway, UDP market data | In progress |
| v4 | Hot standby with failover | Planned |

## Design

- **FastBook**: price-level array, occupancy bitset (`ctz`/`clz` to find the
  next best price), intrusive FIFO per level, fixed slot pool, open-addressing
  id index. No heap allocation after construction.
- **Engine**: logs every command, matches it immediately, and releases its
  result only after its group commit is synced. Snapshots rotate log segments;
  recovery restores the newest snapshot and replays its segment.
- **Server**: a network thread (kqueue/epoll gateway) and an engine thread
  connected by lock-free SPSC rings. Clients get a response only once their
  command is durable; fills are reported to both parties.
- **Market data**: every order event on a sequenced UDP feed; listeners
  rebuild the exact book and recover from any lost packet with a TCP snapshot.
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

Durability ([details](bench/RESULTS.md#engine-durability-v2)): with group commit
of 1,024 commands, 8.0M ops/s with `fsync` and 275K ops/s with `F_FULLFSYNC`
(full SSD flush; about 760x faster than syncing each command). Recovering a
65K-order snapshot plus a 10K-command log takes 12 ms.

Network ([details](bench/RESULTS.md#network-v3)): a durable order round trip
over localhost TCP takes 19 µs (p50) without sync and 35 µs with `fsync`; four
pipelining clients reach 4.5M durable orders/s.

## Testing

- A shared behavior suite runs on both books, with invariants checked after
  every operation.
- A seeded differential test drives both books with 1M+ operations and requires
  identical fills, rejects, and depth after each one, including at the edges of
  the price range.
- Crash recovery: 10,000 `SIGKILL`s at random points, with zero lost
  acknowledgements and zero state mismatches after recovery; truncating the
  log at any byte recovers, and a damaged record mid-log refuses to start.
- Everything runs under ASan and UBSan in CI (GCC and Clang).

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Add `-DHARBOR_SANITIZE=ON` for sanitizers.

Try it (three terminals):

```bash
./build/harbor_listen --feed-port 9100 --snapshot-port 9001
./build/harbor_server --dir /tmp/harbor-data --feed-port 9100
./build/harbor_client --client 1 --id 1 buy 500000 10
``` Benchmarks: configure with
`-DCMAKE_BUILD_TYPE=Release` and run `./build-rel/harbor_bench`.

MIT licensed.
