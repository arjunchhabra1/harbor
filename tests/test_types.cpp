#include <gtest/gtest.h>
#include "harbor/types.hpp"
using namespace harbor;

TEST(Types, RejectNames) {
    EXPECT_STREQ(toString(Reject::None), "None");
    EXPECT_STREQ(toString(Reject::BookFull), "BookFull");
}

TEST(Types, FillEquality) {
    Fill a{1, 2, 150, 10}, b{1, 2, 150, 10}, c{1, 2, 151, 10};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}
