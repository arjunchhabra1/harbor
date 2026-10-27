#include <gtest/gtest.h>
#include <cstdint>
#include <thread>
#include "harbor/spsc_ring.hpp"
using harbor::SpscRing;

TEST(SpscRing, FullAndEmpty) {
    SpscRing<int, 8> ring;
    int out = -1;
    EXPECT_FALSE(ring.tryPop(out));
    for (int i = 0; i < 8; ++i) {
        EXPECT_TRUE(ring.tryPush(i)) << i;
    }
    EXPECT_FALSE(ring.tryPush(8));
    EXPECT_EQ(ring.sizeApprox(), 8u);
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(ring.tryPop(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_FALSE(ring.tryPop(out));
    EXPECT_EQ(ring.sizeApprox(), 0u);
}

TEST(SpscRing, Wraparound) {
    SpscRing<std::uint64_t, 4> ring;
    std::uint64_t out = 0;
    for (std::uint64_t i = 0; i < 1'000; ++i) {
        ASSERT_TRUE(ring.tryPush(i * 3));
        if (i % 2 == 1) {  // keep 1-2 items queued so indices wrap with data in flight
            ASSERT_TRUE(ring.tryPop(out));
            ASSERT_TRUE(ring.tryPop(out));
            EXPECT_EQ(out, i * 3);
        }
    }
}

TEST(SpscRing, TwoThreadStress) {
#ifdef HARBOR_SANITIZED
    constexpr std::uint64_t kCount = 500'000;
#else
    constexpr std::uint64_t kCount = 5'000'000;
#endif
    static SpscRing<std::uint64_t, 1024> ring;  // static: 8 KB of storage off the stack
    std::thread producer([] {
        for (std::uint64_t i = 0; i < kCount; ++i) {
            while (!ring.tryPush(i)) {
                std::this_thread::yield();
            }
        }
    });
    std::uint64_t expected = 0;
    std::uint64_t value = 0;
    while (expected < kCount) {
        if (ring.tryPop(value)) {
            ASSERT_EQ(value, expected);  // no loss, duplication, or reordering
            ++expected;
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();
    EXPECT_FALSE(ring.tryPop(value));
}
