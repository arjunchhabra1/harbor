#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <vector>
#include "harbor/reference_book.hpp"
using namespace harbor;

constexpr BookConfig kCfg{100, 200, 1, 8};  // prices 100..200, 8 orders max

template <class Book>
class BookTest : public ::testing::Test {
protected:
    Book book{kCfg};
    std::vector<Fill> fills;

    Reject add(OrderId id, Side s, Price p, Quantity q, TimeInForce t = TimeInForce::GTC) {
        Reject r = book.add(NewOrder{id, 1, s, p, q, t}, fills);
        check();
        return r;
    }
    Reject cancel(OrderId id) {
        Reject r = book.cancel(id);
        check();
        return r;
    }
    Reject replace(OrderId id, Price p, Quantity q) {
        Reject r = book.replace(id, p, q, fills);
        check();
        return r;
    }
    void check() {
        std::string e;
        ASSERT_TRUE(book.checkInvariants(e)) << e;
    }
};

using Books = ::testing::Types<ReferenceBook>;
TYPED_TEST_SUITE(BookTest, Books);
constexpr auto B = Side::Buy;
constexpr auto S = Side::Sell;
constexpr auto IOC = TimeInForce::IOC;
using Fills = std::vector<Fill>;

TYPED_TEST(BookTest, RestsWhenNotCrossing) {
    EXPECT_EQ(this->add(1, B, 150, 10), Reject::None);
    EXPECT_TRUE(this->fills.empty());
    EXPECT_EQ(this->add(2, S, 152, 5), Reject::None);
    EXPECT_TRUE(this->fills.empty());
    EXPECT_EQ(this->book.bestBid(), 150);
    EXPECT_EQ(this->book.bestAsk(), 152);
    EXPECT_EQ(this->book.depthAt(B, 150), 10u);
    EXPECT_EQ(this->book.orderCount(), 2u);
}

TYPED_TEST(BookTest, PricePriority) {
    this->add(1, S, 152, 5);
    this->add(2, S, 151, 5);
    EXPECT_EQ(this->add(3, B, 152, 5), Reject::None);
    EXPECT_EQ(this->fills, (Fills{{3, 2, 151, 5}}));
    EXPECT_EQ(this->book.bestAsk(), 152);
}

TYPED_TEST(BookTest, TimePriority) {
    this->add(1, S, 151, 5);
    this->add(2, S, 151, 5);
    this->add(3, B, 151, 5);
    EXPECT_EQ(this->fills, (Fills{{3, 1, 151, 5}}));
    EXPECT_EQ(this->book.depthAt(S, 151), 5u);
}

TYPED_TEST(BookTest, PartialFillRestingKeepsQueue) {
    this->add(1, S, 151, 10);
    this->add(2, S, 151, 10);
    this->add(3, B, 151, 4);
    this->add(4, B, 151, 4);
    EXPECT_EQ(this->fills, (Fills{{4, 1, 151, 4}}));
}

TYPED_TEST(BookTest, PartialFillAggressorRests) {
    this->add(1, S, 151, 5);
    this->add(2, B, 153, 8);
    EXPECT_EQ(this->fills, (Fills{{2, 1, 151, 5}}));
    EXPECT_EQ(this->book.bestBid(), 153);
    EXPECT_EQ(this->book.depthAt(B, 153), 3u);
    EXPECT_EQ(this->book.bestAsk(), std::nullopt);
}

TYPED_TEST(BookTest, MultiLevelSweep) {
    this->add(1, S, 151, 2);
    this->add(2, S, 152, 3);
    this->add(3, S, 153, 4);
    this->add(4, B, 153, 8);
    EXPECT_EQ(this->fills, (Fills{{4, 1, 151, 2}, {4, 2, 152, 3}, {4, 3, 153, 3}}));
    EXPECT_EQ(this->book.depthAt(S, 153), 1u);
}

TYPED_TEST(BookTest, TradeAtRestingPrice) {
    this->add(1, B, 150, 5);
    this->add(2, S, 140, 5);
    EXPECT_EQ(this->fills, (Fills{{2, 1, 150, 5}}));
}

TYPED_TEST(BookTest, IocRemainderDiscarded) {
    this->add(1, S, 151, 3);
    EXPECT_EQ(this->add(2, B, 152, 10, IOC), Reject::None);
    EXPECT_EQ(this->fills, (Fills{{2, 1, 151, 3}}));
    EXPECT_EQ(this->book.bestBid(), std::nullopt);
    EXPECT_EQ(this->book.orderCount(), 0u);
}

TYPED_TEST(BookTest, IocNoCrossNeverRests) {
    EXPECT_EQ(this->add(1, B, 150, 5, IOC), Reject::None);
    EXPECT_TRUE(this->fills.empty());
    EXPECT_EQ(this->book.orderCount(), 0u);
}

TYPED_TEST(BookTest, Cancel) {
    this->add(1, B, 150, 5);
    EXPECT_EQ(this->cancel(1), Reject::None);
    EXPECT_EQ(this->cancel(1), Reject::UnknownOrder);
    EXPECT_EQ(this->cancel(99), Reject::UnknownOrder);
    EXPECT_EQ(this->book.depthAt(B, 150), 0u);
}

TYPED_TEST(BookTest, CancelFilledOrderIsUnknown) {
    this->add(1, S, 151, 5);
    this->add(2, B, 151, 5);
    EXPECT_EQ(this->cancel(1), Reject::UnknownOrder);
}

TYPED_TEST(BookTest, ReplaceReduceKeepsPriority) {
    this->add(1, S, 151, 10);
    this->add(2, S, 151, 10);
    EXPECT_EQ(this->replace(1, 151, 4), Reject::None);
    this->add(3, B, 151, 4);
    EXPECT_EQ(this->fills, (Fills{{3, 1, 151, 4}}));
}

TYPED_TEST(BookTest, ReplaceIncreaseLosesPriority) {
    this->add(1, S, 151, 5);
    this->add(2, S, 151, 5);
    EXPECT_EQ(this->replace(1, 151, 6), Reject::None);
    this->add(3, B, 151, 5);
    EXPECT_EQ(this->fills, (Fills{{3, 2, 151, 5}}));
}

TYPED_TEST(BookTest, ReplacePriceChangeLosesPriority) {
    this->add(1, S, 152, 5);
    this->add(2, S, 151, 5);
    EXPECT_EQ(this->replace(1, 151, 5), Reject::None);
    this->add(3, B, 151, 5);
    EXPECT_EQ(this->fills, (Fills{{3, 2, 151, 5}}));
}

TYPED_TEST(BookTest, ReplaceThatCrosses) {
    this->add(1, B, 150, 5);
    this->add(2, S, 152, 5);
    EXPECT_EQ(this->replace(1, 152, 5), Reject::None);
    EXPECT_EQ(this->fills, (Fills{{1, 2, 152, 5}}));
    EXPECT_EQ(this->book.orderCount(), 0u);
}

TYPED_TEST(BookTest, ReplaceErrors) {
    this->add(1, B, 150, 5);
    EXPECT_EQ(this->replace(99, 150, 5), Reject::UnknownOrder);
    EXPECT_EQ(this->replace(1, 150, 0), Reject::BadQuantity);
    EXPECT_EQ(this->replace(1, 201, 5), Reject::BadPrice);
    EXPECT_EQ(this->replace(1, 150, 5), Reject::None);
    EXPECT_EQ(this->book.depthAt(B, 150), 5u);
}

TYPED_TEST(BookTest, Rejects) {
    EXPECT_EQ(this->add(1, B, 150, 5), Reject::None);
    const std::vector<std::pair<NewOrder, Reject>> cases = {
        {{2, 1, B, 150, 0, TimeInForce::GTC}, Reject::BadQuantity},
        {{3, 1, B, 99, 5, TimeInForce::GTC}, Reject::BadPrice},
        {{4, 1, S, 201, 5, TimeInForce::GTC}, Reject::BadPrice},
        {{1, 1, S, 160, 5, TimeInForce::GTC}, Reject::DuplicateId},
    };
    for (const auto& [order, expected] : cases) {
        this->fills = {Fill{9, 9, 9, 9}};  // must be cleared by the call
        EXPECT_EQ(this->book.add(order, this->fills), expected) << order.id;
        EXPECT_TRUE(this->fills.empty()) << order.id;
        EXPECT_EQ(this->book.orderCount(), 1u) << order.id;
        this->check();
    }
}

TYPED_TEST(BookTest, OffTickPrice) {
    TypeParam book{BookConfig{100, 200, 5, 8}};
    std::vector<Fill> fills;
    EXPECT_EQ(book.add({1, 1, B, 152, 1, TimeInForce::GTC}, fills), Reject::BadPrice);
    EXPECT_EQ(book.add({2, 1, B, 155, 1, TimeInForce::GTC}, fills), Reject::None);
}

TYPED_TEST(BookTest, BookFull) {
    for (OrderId id = 1; id <= 8; ++id) {
        ASSERT_EQ(this->add(id, B, static_cast<Price>(100 + id), 1), Reject::None);
    }
    EXPECT_EQ(this->add(9, B, 100, 1), Reject::BookFull);
    EXPECT_EQ(this->add(10, S, 108, 1), Reject::None);
    EXPECT_EQ(this->fills, (Fills{{10, 8, 108, 1}}));
}

TYPED_TEST(BookTest, BoundaryPrices) {
    this->add(1, B, 100, 1);
    this->add(2, S, 200, 1);
    EXPECT_EQ(this->book.bestBid(), 100);
    EXPECT_EQ(this->book.bestAsk(), 200);
    this->add(3, S, 100, 1);
    EXPECT_EQ(this->fills, (Fills{{3, 1, 100, 1}}));
    EXPECT_EQ(this->cancel(2), Reject::None);
    EXPECT_EQ(this->book.orderCount(), 0u);
    EXPECT_EQ(this->book.bestBid(), std::nullopt);
    EXPECT_EQ(this->book.bestAsk(), std::nullopt);
}

TYPED_TEST(BookTest, SweepEmptiesSide) {
    this->add(1, S, 151, 1);
    this->add(2, S, 152, 1);
    this->add(3, B, 160, 2);
    EXPECT_EQ(this->book.bestAsk(), std::nullopt);
    this->add(4, S, 170, 1);
    EXPECT_EQ(this->book.bestAsk(), 170);
    EXPECT_EQ(this->book.bestBid(), std::nullopt);
}

TYPED_TEST(BookTest, SlotReusedAfterCancel) {
    for (OrderId id = 1; id <= 8; ++id) {
        ASSERT_EQ(this->add(id, B, static_cast<Price>(100 + id), 1), Reject::None);
    }
    EXPECT_EQ(this->cancel(3), Reject::None);
    EXPECT_EQ(this->add(9, B, 100, 1), Reject::None);
    EXPECT_EQ(this->book.orderCount(), 8u);
}

TYPED_TEST(BookTest, IdReuseAfterFillAndCancel) {
    EXPECT_EQ(this->add(1, S, 151, 1), Reject::None);
    EXPECT_EQ(this->add(2, B, 151, 1), Reject::None);
    EXPECT_EQ(this->add(1, B, 140, 1), Reject::None);
    EXPECT_EQ(this->cancel(1), Reject::None);
    EXPECT_EQ(this->add(1, B, 141, 1), Reject::None);
}
