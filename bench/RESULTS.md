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
| Commit | `df13cf0` |

## Workload

10,000,000 operations from `OpGenerator` (seed 42), generated before timing
against a scratch `ReferenceBook` so every cancel and replace targets an order
that is actually resting at that point. Mix: 60% add (20% IOC), 30% cancel,
10% replace, prices clustered around the midpoint with a 10-tick overlap
between buys and sells, and 3% of adds reusing the id of a dead order. Book:
prices 1,000 to 2,000, tick 1, up to 65,536 orders.

## Results (median of five runs, typical percentiles)

| Book | Throughput | ≈ ns/op (from throughput) | p50 | p99 | p99.9 | Allocations in timed runs |
|---|---|---|---|---|---|---|
| **FastBook** | **26.9M ops/s** | **37 ns** | ≤ 42 ns | 84 ns | 167 ns | **0** |
| ReferenceBook | 12.4M ops/s | 81 ns | 83 ns | 292 ns | 500 ns | 8,792,471 (throughput run) |

Range across the five runs: FastBook 26.1M to 27.9M ops/s, ReferenceBook 12.2M
to 12.7M. FastBook's max latency ranged from 1.3 to 8.7 µs; those outliers are
OS scheduling and interrupts, not the book (ReferenceBook's max of about 0.45 ms
also includes allocator work).

An earlier set of runs (commit `99c81f8`, before the workload gained id reuse)
included two FastBook runs at 17.9M and 18.9M ops/s, both the first run after a
build or an idle period. macOS does not let a process pin itself to a
performance core, so a run can start on an efficiency core; expect occasional
slow first runs.

## How to read the latency numbers

On Apple Silicon, `std::chrono::steady_clock` ticks at 24 MHz, so every latency
sample is a multiple of about 41.7 ns (41, 83, 125, 167, ...). A p50 of "41 ns"
means "finished within one timer tick". The median overhead of an empty timed
region measures as 0 ns for the same reason. Throughput is the more precise
figure: 26.9M ops/s is about 37 ns per operation on average. On Linux x86-64,
`steady_clock` has nanosecond resolution and the percentiles will be finer.

FastBook's zero-allocation result is measured, not assumed: the benchmark
replaces global `operator new` with a counting version and reports every heap
allocation made during each timed run.

# Engine durability (v2)

Produced by `bench/bench_engine.cpp` on the same machine (Apple M3, macOS 14.5,
Apple clang 16, `-O2 -march=native`), measured with the code in `4e36b43`, median of
three runs. Commands come from the same generator (seed 42) against a book of up
to 65,536 orders; the caller drains outputs after every submit.

## Throughput by batch size and sync mode

| Sync mode | Batch 1 | Batch 64 | Batch 1,024 |
|---|---|---|---|
| None (no sync call) | 771K ops/s | 8.1M ops/s | 12.0M ops/s |
| Fsync (`fsync`) | 62K ops/s | 0.93M ops/s | 8.0M ops/s |
| Full (`F_FULLFSYNC`) | 361 ops/s | 21K ops/s | 275K ops/s |

Every cell made zero heap allocations after warm-up. Snapshots were disabled in
these runs; taking a snapshot allocates (it builds the snapshot in memory), so
the zero-allocation claim covers steady state between snapshots.

`F_FULLFSYNC` is the only mode that forces the SSD to flush its own cache, so
it is the only one durable against power loss on macOS, and it costs about
3 ms per call. Group commit is what makes it usable: batching 1,024 commands
per sync is about 760x faster than syncing each one.

## Commit latency (64 records per commit)

Time for one `commit()`: the `write` of 64 records (3 KB) plus the sync call.

| Sync mode | p50 | p99 |
|---|---|---|
| None | 5.7 µs | 11.5 µs |
| Fsync | 33 µs | 62 µs |
| Full | 2.96 ms | 4.5 ms |

## Recovery time

| Directory | Recovery time |
|---|---|
| 1M-command log, no snapshot | 190 ms (range 177 to 205) |
| Snapshot of 65,533 orders + ~10K-command log | 12 ms |

Snapshots cut recovery time by about 15x here, and recovery without one grows
linearly with the log. These directories were written and reopened with
`SyncMode::None`, so the times cover reading, checking, and replaying, not the
sync calls recovery makes in the durable modes.
