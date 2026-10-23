#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
#include <vector>
#include "harbor/command.hpp"
#include "harbor/fast_book.hpp"
#include "harbor/reference_book.hpp"
#include "op_generator.hpp"
using namespace harbor;

namespace {

std::uint64_t envOr(const char* name, std::uint64_t fallback) {
    const char* value = std::getenv(name);
    return value == nullptr ? fallback : std::strtoull(value, nullptr, 10);
}

struct Scenario {
    BookConfig cfg;
    Price mid;
    Price halfWindow;
    std::uint64_t defaultOps;
    std::uint64_t checkEvery;  // full depth + invariants of both books
};

void runDifferential(const Scenario& sc) {
    std::uint64_t seed = envOr("HARBOR_SEED", 12345);
    std::uint64_t ops = envOr("HARBOR_DIFF_OPS", sc.defaultOps);
    const BookConfig& cfg = sc.cfg;
    ReferenceBook ref{cfg};
    FastBook fast{cfg};
    harbor::testing::OpGenerator gen(seed, sc.mid, sc.halfWindow, cfg.minPrice, cfg.maxPrice,
                                     cfg.tick);
    std::vector<Fill> rf, ff;
    for (std::uint64_t i = 0; i < ops; ++i) {
        auto op = gen.next([&](OrderId id) { return ref.contains(id); });
        Reject rr = applyCommand(ref, op, rf);
        Reject fr = applyCommand(fast, op, ff);
        ASSERT_EQ(rr, fr) << "seed=" << seed << " op=" << i << " ref=" << toString(rr)
                          << " fast=" << toString(fr);
        ASSERT_EQ(rf, ff) << "seed=" << seed << " op=" << i;
        ASSERT_EQ(ref.bestBid(), fast.bestBid()) << "seed=" << seed << " op=" << i;
        ASSERT_EQ(ref.bestAsk(), fast.bestAsk()) << "seed=" << seed << " op=" << i;
        ASSERT_EQ(ref.orderCount(), fast.orderCount()) << "seed=" << seed << " op=" << i;
        if (i % sc.checkEvery == 0) {
            for (Price p = cfg.minPrice; p <= cfg.maxPrice; p += cfg.tick) {
                for (Side s : {Side::Buy, Side::Sell}) {
                    ASSERT_EQ(ref.depthAt(s, p), fast.depthAt(s, p))
                        << "seed=" << seed << " op=" << i << " p=" << p;
                }
            }
            std::string e;
            ASSERT_TRUE(ref.checkInvariants(e)) << "ref: " << e << " seed=" << seed << " op=" << i;
            ASSERT_TRUE(fast.checkInvariants(e)) << "fast: " << e << " seed=" << seed << " op=" << i;
        }
    }
}

}  // namespace

// Replay a failure with: HARBOR_SEED=<seed> ./build/harbor_tests --gtest_filter='Differential.*'

// A deep book around the midpoint of a wide range: realistic shape, heavy matching.
TEST(Differential, DeepBook) {
    runDifferential({BookConfig{1'000, 2'000, 1, 4096}, 1'500, 100, 1'000'000, 1'000});
}

// A narrow tick-5 range with a 64-order pool, with the order flow centered on
// the bottom, middle, and top of the range in turn. At the ends the best price
// regularly falls to the first or last level and those levels empty and refill;
// off-tick prices are rejected and the pool fills.
TEST(Differential, EdgesTickAndSmallPool) {
    for (Price mid : {100, 250, 400}) {
        SCOPED_TRACE("mid=" + std::to_string(mid));
        runDifferential({BookConfig{100, 400, 5, 64}, mid, 60, 100'000, 1});
        if (HasFatalFailure()) {
            return;
        }
    }
}
