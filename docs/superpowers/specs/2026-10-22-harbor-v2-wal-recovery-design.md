# Harbor v2: write-ahead log and crash recovery

Date: 2026-10-22
Status: design approved in conversation, awaiting written-spec review
Builds on: `2026-10-22-harbor-v1-design.md` (merged to `main`)

## Purpose

If the Harbor process crashes or is killed, restarting it rebuilds exactly the
same order book, and every result that was ever acknowledged is still true.

v1's matching is deterministic: the same commands in the same order always
produce the same book, fills, and rejects. v2 therefore logs **commands**
(command sourcing), not results, and recovers by loading a snapshot and
replaying the commands logged after it.

### Success criteria

1. Every command acknowledged before a crash is reflected after recovery, and
   the recovered book equals a reference book that applied exactly the durable
   commands.
2. 200 real `SIGKILL` crashes in the regular test suite and 10,000 in a separate
   tool, with zero lost acknowledgements and zero state mismatches.
3. Truncating the log at every byte offset of its last records always recovers;
   corrupting a record in the middle always fails closed.
4. Recovery succeeds from a crash after each step of snapshot writing.
5. Steady-state `submit` performs zero heap allocations.
6. Benchmarks report throughput for each sync mode at batch sizes 1, 64, and
   1,024, commit latency, and recovery time.
7. All tests pass under ASan + UBSan (no recovery) and in CI.

### Out of scope

Threads, networking, multiple symbols, compression, log replication (v4), and
redelivering outputs to callers after a restart (v3: clients reconcile using
`lastDurableSeq()`). Durability beyond what the OS sync call guarantees, such
as disk firmware bugs, is not claimed.

## Decisions

| Decision | Choice |
|---|---|
| What is logged | Every command, including ones that will be rejected |
| Durability | Group commit: one write + sync per batch |
| Ordering of apply vs sync | Apply immediately, release outputs only after their batch is durable (LMAX style) |
| Snapshots | Every N commands, inline, write-temp-then-rename |
| Log storage | One segment per snapshot; older snapshot and segment deleted after a newer snapshot is durable |
| Restore | Re-add snapshot orders through the book's public `add()` in price-time order |

## Core types

```cpp
namespace harbor {

struct Command {
    enum class Kind : std::uint8_t { Add = 1, Cancel = 2, Replace = 3 } kind;
    NewOrder order;                    // Add
    OrderId id;                        // Cancel / Replace
    Price price;                       // Replace
    Quantity qty;                      // Replace
};

struct Output {
    std::uint64_t seq;                 // the command's log position, starting at 1
    Reject result;
    std::span<const Fill> fills;       // valid until drain() returns
};

enum class SyncMode { Full, Fsync, None };

struct EngineOptions {
    std::filesystem::path dir;
    std::uint32_t maxBatch = 1024;
    std::chrono::microseconds maxDelay{1000};
    std::uint64_t snapshotEvery = 1'000'000;
    SyncMode sync = SyncMode::Full;
};

}  // namespace harbor
```

- `SyncMode::Full`: `fcntl(fd, F_FULLFSYNC)` on macOS, `fdatasync` on Linux.
  The only mode that is durable against power loss on macOS.
- `SyncMode::Fsync`: `fsync`.
- `SyncMode::None`: no sync call. Survives a process crash (the page cache
  survives) but not power loss. For tests and benchmarks.

Directory syncs (after create, rename, delete) use `fsync` on the directory in
`Full` and `Fsync` modes and are skipped in `None`.

## Engine

```cpp
template <class Book>
class Engine {
public:
    using Clock = std::chrono::steady_clock;

    // Recovers from options.dir if it contains data, otherwise starts empty.
    // Throws std::runtime_error on corruption or I/O failure, and
    // std::invalid_argument if cfg differs from the config stored in the snapshot.
    Engine(const BookConfig& cfg, const EngineOptions& options);

    std::uint64_t submit(const Command& cmd, Clock::time_point now);  // returns seq
    void poll(Clock::time_point now);
    void commit();
    template <class F> void drain(F&& onOutput);  // calls onOutput(const Output&) in seq order

    const Book& book() const;
    std::uint64_t lastSeq() const;          // last submitted
    std::uint64_t lastDurableSeq() const;   // last synced
};
```

`submit` takes the time so the engine never reads a clock itself; tests are
deterministic.

### Flow of one command

1. Assign `seq = lastSeq + 1`, encode the 48-byte record into the in-memory
   batch buffer. If this is the first pending command, record `now` as the
   batch start time.
2. Apply the command to the book.
3. Append the result and its fills to the pending output buffers.
4. If pending count reaches `maxBatch`, `commit()`.

`poll(now)` commits if there are pending commands and
`now - batchStart >= maxDelay`.

`commit()` writes the batch buffer with one `write` call (looping on short
writes), syncs per `SyncMode`, sets `lastDurableSeq = lastSeq`, and moves pending
outputs to released. Then, if at least `snapshotEvery` commands have been
committed since the last snapshot, it takes a snapshot.

`drain(f)` calls `f` for each released output in seq order and clears them.
Pending (not yet durable) outputs are never visible.

An I/O error during `commit` throws `std::runtime_error`; the engine is then
unusable (subsequent calls throw) because the book has moved ahead of the
durable log. Recovery from the directory is the way forward.

### Allocation discipline

The batch buffer (`maxBatch × 48` bytes), the output buffer (`maxBatch`
entries), and the fill buffer are allocated in the constructor. The fill buffer
reserves `maxBatch × 4` fills; a batch that exceeds it grows the buffer once
(amortized, not steady state). Steady-state `submit` performs zero heap
allocations; the benchmark verifies this.

## Files on disk

```
<dir>/
  snap-NNNNNNNNNNNN.bin    book state after command seq S (segment N)
  wal-NNNNNNNNNNNN.log     commands S+1, S+2, ... (segment N)
```

`N` is a 12-digit zero-padded segment number. Segment 0 has no snapshot (it
starts from an empty book). All integers are encoded little-endian, field by
field.

### Log segment header (32 bytes)

| Offset | Field | Type |
|---|---|---|
| 0 | magic `"HWAL"` | 4 bytes |
| 4 | version (1) | u16 |
| 6 | reserved (0) | u16 |
| 8 | segment number | u64 |
| 16 | first seq in this segment | u64 |
| 24 | reserved (0) | u32 |
| 28 | CRC32C of bytes 0–27 | u32 |

### Log record (48 bytes)

| Offset | Field | Type |
|---|---|---|
| 0 | seq | u64 |
| 8 | kind (1 add, 2 cancel, 3 replace) | u8 |
| 9 | side (0 buy, 1 sell) | u8 |
| 10 | tif (0 GTC, 1 IOC) | u8 |
| 11 | reserved (0) | u8 |
| 12 | client | u32 |
| 16 | order id | u64 |
| 24 | price | i64 (two's complement) |
| 32 | qty | u32 |
| 36 | reserved (0) | u64 |
| 44 | CRC32C of bytes 0–43 | u32 |

Cancel stores only the id (other fields 0). Replace stores id, price, qty.

### Snapshot file

Header (56 bytes): magic `"HSNP"` (4), version 1 (u16), reserved (u16),
segment number (u64), last seq (u64), minPrice (i64), maxPrice (i64),
tick (i64), maxOrders (u32), order count (u32).

Then one 32-byte entry per resting order: id (u64), client (u32), side (u8),
3 reserved bytes, price (i64), remaining (u32), reserved (u32).

Then CRC32C (u32) over every preceding byte of the file.

Orders are written by `forEachResting`: bids from best price to worst, then
asks from best to worst, each level in queue order.

### Restoring a snapshot

Construct an empty book with the snapshot's config and call
`add({id, client, side, price, remaining, GTC})` for each entry in file order.
Every call must return `Reject::None` with no fills; anything else is
corruption. Because the snapshotted book was never crossed and each level is
re-added in queue order, the result is identical in every observable way.

### Book addition

Both books gain:

```cpp
// Visits resting orders: bids best price first, then asks best price first,
// each level in time priority. f(OrderId, ClientId, Side, Price, Quantity remaining).
template <class F> void forEachResting(F&& f) const;
```

### Writing a snapshot (segment N → N+1)

1. Write `snap-(N+1).tmp`; sync it.
2. Rename to `snap-(N+1).bin`; sync the directory.
3. Create `wal-(N+1).log` with its header; sync it and the directory.
4. Delete `snap-N.bin` (if any) and `wal-N.log`; sync the directory.

A crash between any two steps leaves at least one complete snapshot (or
segment 0) and a usable log. Stray `*.tmp` files are deleted at startup.

## Recovery

On construction, with `dir` containing files:

1. Delete `*.tmp`.
2. Pick the highest-numbered `snap-*.bin`; if none, start from segment 0 with an
   empty book. Verify its CRC and that its config equals `cfg`. CRC failure →
   `std::runtime_error` (a snapshot cannot be half-written, so this is real
   corruption). Config mismatch → `std::invalid_argument`.
3. Restore the snapshot as described above.
4. Open `wal-N.log` for the same segment. If it does not exist (crash between
   snapshot steps 2 and 3), create it. Verify its header CRC, segment number,
   and that first seq equals snapshot last seq + 1.
5. Read records in order. For each valid record, require `seq` to equal the
   expected next seq (else `std::runtime_error`), then apply it to the book.
6. Torn tail handling, at the first record that is incomplete or fails its CRC:
   - If every remaining complete 48-byte slot after it also fails its CRC (or
     there are none), truncate the file at that record and stop: this is an
     unacknowledged torn write.
   - If any later slot passes its CRC, throw `std::runtime_error`: corruption in
     the middle of the log.
7. Delete any segment files numbered below N.
8. `lastSeq = lastDurableSeq = last applied seq`. Outputs from replay are
   discarded (they were either delivered before the crash or never
   acknowledged).

## Testing

1. **CRC32C**: `"123456789"` → `0xE3069283`; empty input → `0`.
2. **Record codec**: round trip of every kind, with negative and extreme prices,
   id 0, IOC; reserved bytes are zero.
3. **Log writer/reader**: round trip; partial last record truncated; bad
   records to end truncated; bad record followed by a good one fails; seq gap
   fails; bad header fails.
4. **Snapshot**: round trip on both books (equal `forEachResting` output, best
   prices, depth); flipped byte fails; empty book; config mismatch.
5. **Engine output gating**: nothing drained before commit; `maxBatch` triggers
   commit; `poll` commits at `maxDelay` and not before; outputs in seq order with
   the right fills; `lastDurableSeq` advances only on commit.
6. **Recovery equivalence**: run N generated commands through
   `Engine<FastBook>`, abandon it without shutdown, reopen; recovered book equals
   a `ReferenceBook` that applied commands 1..`lastDurableSeq`. Run with and
   without snapshots having occurred.
7. **Real crash test**: fork a child running `Engine<FastBook>` (sync `None`) on a
   seeded workload, reporting `lastDurableSeq` over a pipe after each commit;
   the parent sends `SIGKILL` after a random delay, recovers, and checks
   recovered seq ≥ last reported seq and state equality with a reference book
   replayed from the same seed. 200 iterations in the suite;
   `tools/crash_matrix` runs 10,000.
8. **Torn-write matrix**: truncate a real log at every byte offset across its
   last three records → recovery succeeds at the last complete record. Flip a
   byte in each of the first records → recovery fails.
9. **Snapshot crash points**: a test-only hook in `EngineOptions`
   (`std::function<void(int step)> afterSnapshotStep`) throws after each of
   steps 1–4; recovery from the directory must succeed and equal the reference.

## Benchmarks

`bench/bench_engine.cpp`, Release:

- Throughput with logging for `maxBatch` ∈ {1, 64, 1024} × `SyncMode` ∈
  {Full, Fsync, None}, on a fixed number of generated commands per cell (fewer
  for batch 1 with real syncs, so a run finishes in seconds).
- Commit latency: p50 and p99 of the sync call.
- Recovery time: 1M-command log with no snapshot; snapshot of ~65K orders plus
  a 10K-command log.
- Allocations during steady-state `submit` (expected 0).

Results go to `bench/RESULTS.md` and the README with machine details.

## Layout

```
include/harbor/
  crc32c.hpp   command.hpp   wal.hpp   snapshot.hpp   engine.hpp
src/
  crc32c.cpp   command.cpp   wal.cpp   snapshot.cpp
tests/
  test_crc32c.cpp   test_command_codec.cpp   test_wal.cpp   test_snapshot.cpp
  test_engine.cpp   test_recovery.cpp   test_crash.cpp
tools/
  crash_matrix.cpp
bench/
  bench_engine.cpp
```

`tests/op_generator.hpp` switches from its own `Op` type to `harbor::Command`.

## Build order

1. CRC32C and the command record codec.
2. Log writer and reader with torn-tail rules.
3. `forEachResting` on both books; snapshot write/read/restore.
4. Engine with group commit and output gating.
5. Recovery, snapshot rotation, and crash-point hook.
6. Real crash test, torn-write matrix, `crash_matrix` tool.
7. Engine benchmarks and `RESULTS.md`.

The README is kept as brief as possible: what Harbor is, headline numbers, how
to build and test, and links to `docs/` and `bench/RESULTS.md` for detail. It is
trimmed at the start of v2 and updated in the same commit as each step that
changes what it says.
