#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
#include <vector>
#include "harbor/fast_book.hpp"
#include "harbor/reference_book.hpp"
#include "op_generator.hpp"
using namespace harbor;

namespace {

std::uint64_t envOr(const char* name, std::uint64_t fallback) {
    const char* value = std::getenv(name);
    return value == nullptr ? fallback : std::strtoull(value, nullptr, 10);
}

template <class Book>
Reject apply(Book& book, const harbor::testing::Op& op, std::vector<Fill>& out) {
    using Kind = harbor::testing::Op::Kind;
    switch (op.kind) {
        case Kind::Add:
            return book.add(op.order, out);
        case Kind::Cancel:
            out.clear();
            return book.cancel(op.id);
        case Kind::Replace:
            return book.replace(op.id, op.price, op.qty, out);
    }
    return Reject::None;
}

}  // namespace

// Replay a failure with: HARBOR_SEED=<seed> ./build/harbor_tests --gtest_filter='Differential.*'
TEST(Differential, ReferenceAndFastAgree) {
    std::uint64_t seed = envOr("HARBOR_SEED", 12345);
    std::uint64_t ops = envOr("HARBOR_DIFF_OPS", 1'000'000);
    BookConfig cfg{1'000, 2'000, 1, 4096};
    ReferenceBook ref{cfg};
    FastBook fast{cfg};
    harbor::testing::OpGenerator gen(seed, 1'500, 100, cfg.minPrice, cfg.maxPrice);
    std::vector<Fill> rf, ff;
    for (std::uint64_t i = 0; i < ops; ++i) {
        auto op = gen.next([&](OrderId id) { return ref.contains(id); });
        Reject rr = apply(ref, op, rf);
        Reject fr = apply(fast, op, ff);
        ASSERT_EQ(rr, fr) << "seed=" << seed << " op=" << i << " ref=" << toString(rr)
                          << " fast=" << toString(fr);
        ASSERT_EQ(rf, ff) << "seed=" << seed << " op=" << i;
        ASSERT_EQ(ref.bestBid(), fast.bestBid()) << "seed=" << seed << " op=" << i;
        ASSERT_EQ(ref.bestAsk(), fast.bestAsk()) << "seed=" << seed << " op=" << i;
        ASSERT_EQ(ref.orderCount(), fast.orderCount()) << "seed=" << seed << " op=" << i;
        if (i % 1000 == 0) {
            for (Price p = 1'400; p <= 1'600; ++p) {
                for (Side s : {Side::Buy, Side::Sell}) {
                    ASSERT_EQ(ref.depthAt(s, p), fast.depthAt(s, p))
                        << "seed=" << seed << " op=" << i << " p=" << p;
                }
            }
            std::string e;
            ASSERT_TRUE(fast.checkInvariants(e)) << e << " seed=" << seed << " op=" << i;
        }
    }
}
