// Throughput, per-operation latency, and heap-allocation counts for both books.
// Build in Release: cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "harbor/command.hpp"
#include "harbor/fast_book.hpp"
#include "harbor/reference_book.hpp"
#include "alloc_counter.hpp"
#include "op_generator.hpp"

using namespace harbor;
using harbor::testing::Op;

namespace {

constexpr BookConfig kCfg{1'000, 2'000, 1, 65'536};
constexpr std::size_t kThroughputOps = 10'000'000;
constexpr std::size_t kWarmupOps = 100'000;
constexpr std::size_t kLatencyOps = 1'000'000;

// Ops are generated against a scratch ReferenceBook so cancel and replace
// targets are orders that are actually resting at that point in the stream.
std::vector<Op> generate(std::size_t count) {
    std::vector<Op> ops;
    ops.reserve(count);
    ReferenceBook scratch{kCfg};
    harbor::testing::OpGenerator gen(42, 1'500, 100, kCfg.minPrice, kCfg.maxPrice);
    std::vector<Fill> fills;
    for (std::size_t i = 0; i < count; ++i) {
        ops.push_back(gen.next([&](OrderId id) { return scratch.contains(id); }));
        applyCommand(scratch, ops.back(), fills);
    }
    return ops;
}

std::string withCommas(std::uint64_t n) {
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) {
        s.insert(static_cast<std::size_t>(i), ",");
    }
    return s;
}

template <class Book>
void run(const char* name, const std::vector<Op>& ops) {
    using Clock = std::chrono::steady_clock;
    std::vector<Fill> fills;
    fills.reserve(4096);

    {
        Book book{kCfg};
        std::uint64_t before = harbor::bench::allocationCount();
        auto start = Clock::now();
        for (const Op& op : ops) {
            applyCommand(book, op, fills);
        }
        double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        std::uint64_t allocs = harbor::bench::allocationCount() - before;
        std::printf("%-14s throughput   %14s ops/s   (%s ops, %s allocations)\n", name,
                    withCommas(static_cast<std::uint64_t>(ops.size() / seconds)).c_str(),
                    withCommas(ops.size()).c_str(), withCommas(allocs).c_str());
    }

    {
        Book book{kCfg};
        for (std::size_t i = 0; i < kWarmupOps; ++i) {
            applyCommand(book, ops[i], fills);
        }
        std::vector<std::int64_t> samples(kLatencyOps);
        std::uint64_t before = harbor::bench::allocationCount();
        for (std::size_t i = 0; i < kLatencyOps; ++i) {
            auto start = Clock::now();
            applyCommand(book, ops[kWarmupOps + i], fills);
            auto end = Clock::now();
            samples[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        }
        std::uint64_t allocs = harbor::bench::allocationCount() - before;
        std::sort(samples.begin(), samples.end());
        auto pct = [&](double p) {
            return samples[static_cast<std::size_t>(p * static_cast<double>(samples.size() - 1))];
        };
        std::printf("%-14s latency ns   p50 %lld   p99 %lld   p99.9 %lld   max %lld   (%s allocations)\n",
                    name, static_cast<long long>(pct(0.50)), static_cast<long long>(pct(0.99)),
                    static_cast<long long>(pct(0.999)), static_cast<long long>(samples.back()),
                    withCommas(allocs).c_str());
    }
}

}  // namespace

// Median cost of an empty timed region, i.e. what steady_clock adds to every
// latency sample.
std::int64_t clockOverheadNs() {
    using Clock = std::chrono::steady_clock;
    std::vector<std::int64_t> samples(1'000'000);
    for (auto& sample : samples) {
        auto start = Clock::now();
        auto end = Clock::now();
        sample = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

int main() {
    std::printf("steady_clock overhead (median of empty timed region): %lld ns\n",
                static_cast<long long>(clockOverheadNs()));
    std::printf("generating %s ops...\n", withCommas(kThroughputOps).c_str());
    std::vector<Op> ops = generate(kThroughputOps);
    run<FastBook>("FastBook", ops);
    run<ReferenceBook>("ReferenceBook", ops);
    return 0;
}
