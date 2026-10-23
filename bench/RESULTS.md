# Benchmark results

Produced by `bench/bench_books.cpp`:

```bash
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j --target harbor_bench
./build-rel/harbor_bench
```

## Machine

| | |
|---|---|
| CPU | Apple M3 |
| OS | macOS 14.5 |
| Compiler | Apple clang 16.0.0 (clang-1600.0.26.6) |
| Flags | `-O2 -march=native`, C++20 |
| Date | 2026-10-22 |
| Commit | measured on top of `99c81f8` |

## Workload

10,000,000 operations from `OpGenerator` (seed 42), generated before timing
against a scratch `ReferenceBook` so every cancel and replace targets an order
that is actually resting at that point. Mix: 60% add (20% IOC), 30% cancel,
10% replace, prices clustered around the midpoint with a 10-tick overlap
between buys and sells. Book: prices 1,000 to 2,000, tick 1, up to 65,536 orders.

## Results (three runs; median throughput, typical percentiles)

| Book | Throughput | ≈ ns/op (from throughput) | p50 | p99 | p99.9 | Allocations in timed runs |
|---|---|---|---|---|---|---|
| **FastBook** | **26.4M ops/s** | **38 ns** | ≤ 42 ns | 84 ns | 167 ns | **0** |
| ReferenceBook | 10.7M ops/s | 93 ns | 83 ns | 292 ns | 541 ns | 8,793,133 (throughput run) |

Range across runs: FastBook 24.2M to 27.0M ops/s, ReferenceBook 9.6M to 11.1M.
FastBook's max latency ranged from 10 to 31 µs; those outliers are OS
scheduling and interrupts, not the book (ReferenceBook's max of about 0.5 ms
also includes allocator work).

## How to read the latency numbers

On Apple Silicon, `std::chrono::steady_clock` ticks at 24 MHz, so every latency
sample is a multiple of about 41.7 ns (41, 83, 125, 167, ...). A p50 of "41 ns"
means "finished within one timer tick". The median overhead of an empty timed
region measures as 0 ns for the same reason. Throughput is the more precise
figure: 26.4M ops/s is about 38 ns per operation on average. On Linux x86-64,
`steady_clock` has nanosecond resolution and the percentiles will be finer.

FastBook's zero-allocation result is measured, not assumed: the benchmark
replaces global `operator new` with a counting version and reports every heap
allocation made during each timed run.
