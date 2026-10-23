// Durability cost: engine throughput by batch size and sync mode, commit
// latency, recovery time, and steady-state allocations.
// Build in Release: cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
#include "alloc_counter.hpp"
#include "harbor/command.hpp"
#include "harbor/engine.hpp"
#include "harbor/fast_book.hpp"
#include "harbor/reference_book.hpp"
#include "op_generator.hpp"
#include "temp_dir.hpp"

using namespace harbor;
using Clock = std::chrono::steady_clock;

namespace {

constexpr BookConfig kCfg{1'000, 2'000, 1, 65'536};

const char* name(SyncMode m) {
    switch (m) {
        case SyncMode::Full: return "Full";
        case SyncMode::Fsync: return "Fsync";
        case SyncMode::None: return "None";
    }
    return "?";
}

std::vector<Command> generate(std::size_t count) {
    std::vector<Command> cmds;
    cmds.reserve(count);
    ReferenceBook scratch{kCfg};
    harbor::testing::OpGenerator gen(42, 1'500, 100, kCfg.minPrice, kCfg.maxPrice);
    std::vector<Fill> fills;
    for (std::size_t i = 0; i < count; ++i) {
        cmds.push_back(gen.next([&](OrderId id) { return scratch.contains(id); }));
        applyCommand(scratch, cmds.back(), fills);
    }
    return cmds;
}

EngineOptions options(const std::filesystem::path& dir, std::uint32_t maxBatch, SyncMode mode) {
    EngineOptions o;
    o.dir = dir;
    o.maxBatch = maxBatch;
    o.sync = mode;
    o.snapshotEvery = 1'000'000'000;  // measured separately
    return o;
}

void throughput(const std::vector<Command>& cmds, std::uint32_t maxBatch, SyncMode mode, std::size_t count) {
    harbor::testing::TempDir dir;
    Engine<FastBook> engine(kCfg, options(dir.path(), maxBatch, mode));
    std::size_t warmup = std::min<std::size_t>(count / 10, 10'000);
    for (std::size_t i = 0; i < warmup; ++i) {
        engine.submit(cmds[i], Clock::now());
        engine.drain([](const Output&) {});
    }
    std::uint64_t allocsBefore = harbor::bench::allocationCount();
    auto start = Clock::now();
    for (std::size_t i = warmup; i < count; ++i) {
        engine.submit(cmds[i], start);
        engine.drain([](const Output&) {});
    }
    engine.commit();
    engine.drain([](const Output&) {});
    double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    std::uint64_t allocs = harbor::bench::allocationCount() - allocsBefore;
    std::printf("throughput  batch %-5u sync %-5s  %12.0f ops/s  (%zu ops, %llu allocations)\n", maxBatch,
                name(mode), static_cast<double>(count - warmup) / seconds, count - warmup,
                static_cast<unsigned long long>(allocs));
}

void commitLatency(const std::vector<Command>& cmds, SyncMode mode) {
    harbor::testing::TempDir dir;
    Engine<FastBook> engine(kCfg, options(dir.path(), 1u << 20, mode));
    constexpr std::size_t kSamples = 2'000;
    constexpr std::size_t kBatch = 64;
    std::vector<double> micros;
    micros.reserve(kSamples);
    std::size_t next = 0;
    for (std::size_t s = 0; s < kSamples; ++s) {
        for (std::size_t i = 0; i < kBatch; ++i) {
            engine.submit(cmds[next++ % cmds.size()], Clock::now());
        }
        auto start = Clock::now();
        engine.commit();
        micros.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
        engine.drain([](const Output&) {});
    }
    std::sort(micros.begin(), micros.end());
    std::printf("commit      64 records sync %-5s  p50 %8.1f us   p99 %8.1f us\n", name(mode),
                micros[micros.size() / 2], micros[micros.size() * 99 / 100]);
}

void recoveryTime(const std::vector<Command>& cmds, std::size_t count, std::uint64_t snapshotEvery,
                  const char* label) {
    harbor::testing::TempDir dir;
    EngineOptions o = options(dir.path(), 1024, SyncMode::None);
    o.snapshotEvery = snapshotEvery;
    {
        Engine<FastBook> engine(kCfg, o);
        for (std::size_t i = 0; i < count; ++i) {
            engine.submit(cmds[i], Clock::now());
            engine.drain([](const Output&) {});
        }
        engine.commit();
    }
    auto start = Clock::now();
    Engine<FastBook> recovered(kCfg, o);
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    std::printf("recovery    %-38s %8.1f ms  (%zu resting orders, seq %llu)\n", label, ms,
                recovered.book().orderCount(), static_cast<unsigned long long>(recovered.lastDurableSeq()));
}

}  // namespace

int main() {
    std::printf("generating 2,000,000 commands...\n");
    std::vector<Command> cmds = generate(2'000'000);

    for (SyncMode mode : {SyncMode::None, SyncMode::Fsync, SyncMode::Full}) {
        for (std::uint32_t batch : {1u, 64u, 1024u}) {
            std::size_t count = 2'000'000;
            if (mode != SyncMode::None && batch == 1) {
                count = 2'000;
            } else if (mode != SyncMode::None && batch == 64) {
                count = 200'000;
            }
            throughput(cmds, batch, mode, count);
        }
    }
    for (SyncMode mode : {SyncMode::None, SyncMode::Fsync, SyncMode::Full}) {
        commitLatency(cmds, mode);
    }
    recoveryTime(cmds, 1'000'000, 1'000'000'000, "1M-command log, no snapshot");
    recoveryTime(cmds, 1'000'000, 990'000, "snapshot + ~10K-command log");
    return 0;
}
