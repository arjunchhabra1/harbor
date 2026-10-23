#include <gtest/gtest.h>
#include <random>
#include <unordered_map>
#include "harbor/flat_hash_map.hpp"
using harbor::FlatHashMap;

TEST(FlatHashMap, CapacityIsPowerOfTwoAtLeastDouble) {
    FlatHashMap<int> m(100);
    EXPECT_EQ(m.capacity(), 256u);
}

TEST(FlatHashMap, InsertFindErase) {
    FlatHashMap<int> m(10);
    EXPECT_TRUE(m.insert(42, 7));
    ASSERT_NE(m.find(42), nullptr);
    EXPECT_EQ(*m.find(42), 7);
    EXPECT_FALSE(m.insert(42, 8));  // duplicate
    EXPECT_TRUE(m.erase(42));
    EXPECT_EQ(m.find(42), nullptr);
    EXPECT_FALSE(m.erase(42));
    EXPECT_EQ(m.size(), 0u);
}

TEST(FlatHashMap, KeyZero) {
    FlatHashMap<int> m(4);
    EXPECT_EQ(m.find(0), nullptr);
    EXPECT_TRUE(m.insert(0, 5));
    EXPECT_EQ(*m.find(0), 5);
    EXPECT_TRUE(m.erase(0));
    EXPECT_EQ(m.find(0), nullptr);
}

TEST(FlatHashMap, RejectsWhenFull) {
    FlatHashMap<int> m(3);
    EXPECT_TRUE(m.insert(1, 1));
    EXPECT_TRUE(m.insert(2, 2));
    EXPECT_TRUE(m.insert(3, 3));
    EXPECT_FALSE(m.insert(4, 4));
    EXPECT_TRUE(m.erase(2));
    EXPECT_TRUE(m.insert(4, 4));
}

// Fill to the max and churn: erases in the middle of clusters and at the
// table's wraparound point happen many times. Compared against std::unordered_map.
TEST(FlatHashMap, EraseWithWraparound) {
    constexpr std::size_t N = 64;
    FlatHashMap<std::uint64_t> m(N);
    std::unordered_map<std::uint64_t, std::uint64_t> ref;
    std::mt19937_64 rng(1);
    for (int i = 0; i < 200000; ++i) {
        std::uint64_t k = rng() % 256;
        if (rng() % 2 && ref.size() < N) {
            EXPECT_EQ(m.insert(k, k * 3), ref.emplace(k, k * 3).second);
        } else {
            EXPECT_EQ(m.erase(k), ref.erase(k) == 1);
        }
        ASSERT_EQ(m.size(), ref.size());
    }
    for (std::uint64_t k = 0; k < 256; ++k) {
        auto it = ref.find(k);
        if (it == ref.end()) {
            EXPECT_EQ(m.find(k), nullptr) << k;
        } else {
            ASSERT_NE(m.find(k), nullptr) << k;
            EXPECT_EQ(*m.find(k), it->second);
        }
    }
}
