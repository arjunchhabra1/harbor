#include <gtest/gtest.h>
#include "harbor/crc32c.hpp"
using harbor::Crc32c;
using harbor::crc32c;

TEST(Crc32c, KnownVector) {
    EXPECT_EQ(crc32c("123456789", 9), 0xE3069283u);
}

TEST(Crc32c, Empty) {
    EXPECT_EQ(crc32c("", 0), 0u);
}

TEST(Crc32c, IncrementalMatchesOneShot) {
    const char* data = "The quick brown fox jumps over the lazy dog";
    Crc32c c;
    c.update(data, 10);
    c.update(data + 10, 20);
    c.update(data + 30, 13);
    EXPECT_EQ(c.value(), crc32c(data, 43));
}
