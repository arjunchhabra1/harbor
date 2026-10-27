#include <gtest/gtest.h>
#include <chrono>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>
#include "harbor/feed.hpp"
#include "op_generator.hpp"
#include "temp_dir.hpp"
using namespace harbor;
using wire::ExecutionMsg;
using wire::FeedEvent;
using wire::ResponseMsg;
using Type = FeedEvent::Type;

namespace {

constexpr BookConfig kCfg{100, 200, 1, 64};
using Clock = std::chrono::steady_clock;

OrderId oid(ClientId client, std::uint32_t clientOrderId) {
    return (OrderId{client} << 32) | clientOrderId;
}

Command add(OrderId id, Side side, Price price, Quantity qty, TimeInForce tif = TimeInForce::GTC) {
    return Command{Command::Kind::Add, NewOrder{id, clientOf(id), side, price, qty, tif}, 0, 0, 0};
}
Command cancel(OrderId id) { return Command{Command::Kind::Cancel, {}, id, 0, 0}; }
Command replace(OrderId id, Price price, Quantity qty) { return Command{Command::Kind::Replace, {}, id, price, qty}; }

// Runs commands through a FeedEngine and records everything it emits.
class FeedTest : public ::testing::Test {
protected:
    harbor::testing::TempDir dir;
    std::optional<FeedEngine> fe;
    std::vector<FeedEvent> events;
    std::vector<std::pair<ClientId, ResponseMsg>> responses;
    std::vector<std::pair<ClientId, ExecutionMsg>> executions;

    void SetUp() override {
        EngineOptions o;
        o.dir = dir.path();
        o.maxBatch = 1'000;
        o.sync = SyncMode::None;
        fe.emplace(kCfg, o);
    }

    void run(std::initializer_list<Command> cmds) {
        events.clear();
        responses.clear();
        executions.clear();
        for (const Command& c : cmds) {
            fe->submit(c, Clock::time_point{});
        }
        fe->commit();
        fe->drain([&](const FeedEvent& e) { events.push_back(e); },
                  [&](const ResponseMsg& r, ClientId c, std::uint64_t) { responses.emplace_back(c, r); },
                  [&](const ExecutionMsg& x, ClientId c) { executions.emplace_back(c, x); });
    }
};

FeedEvent ev(Type t, std::uint64_t seq, OrderId id, Side side, Price price, Quantity qty, std::uint64_t match) {
    return FeedEvent{t, seq, id, side, price, qty, match};
}

}  // namespace

TEST_F(FeedTest, RestEmitsAdded) {
    run({add(oid(1, 5), Side::Buy, 150, 10)});
    EXPECT_EQ(events, (std::vector<FeedEvent>{ev(Type::Added, 1, oid(1, 5), Side::Buy, 150, 10, 0)}));
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_EQ(responses[0].first, 1u);
    EXPECT_EQ(responses[0].second, (ResponseMsg{5, 2, Reject::None, 1, 10}));
    EXPECT_TRUE(executions.empty());
    EXPECT_EQ(fe->lastFeedSeq(), 1u);
}

TEST_F(FeedTest, SweepEmitsExecutedThenAdded) {
    run({add(oid(1, 1), Side::Sell, 151, 3), add(oid(2, 1), Side::Sell, 152, 4)});
    run({add(oid(3, 9), Side::Buy, 153, 10)});
    EXPECT_EQ(events, (std::vector<FeedEvent>{
                          ev(Type::Executed, 3, oid(1, 1), Side::Buy, 151, 3, 1),
                          ev(Type::Executed, 4, oid(2, 1), Side::Buy, 152, 4, 2),
                          ev(Type::Added, 5, oid(3, 9), Side::Buy, 153, 3, 0),
                      }));
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_EQ(responses[0], (std::pair{ClientId{3}, ResponseMsg{9, 2, Reject::None, 3, 3}}));
    EXPECT_EQ(executions, (std::vector<std::pair<ClientId, ExecutionMsg>>{
                              {3, ExecutionMsg{9, 3, 151, 3, ExecutionMsg::Aggressor, 1}},
                              {1, ExecutionMsg{1, 3, 151, 3, ExecutionMsg::Resting, 1}},
                              {3, ExecutionMsg{9, 3, 152, 4, ExecutionMsg::Aggressor, 2}},
                              {2, ExecutionMsg{1, 3, 152, 4, ExecutionMsg::Resting, 2}},
                          }));
}

TEST_F(FeedTest, IocRemainderNotAdded) {
    run({add(oid(1, 1), Side::Sell, 151, 3)});
    run({add(oid(2, 1), Side::Buy, 155, 10, TimeInForce::IOC)});
    EXPECT_EQ(events, (std::vector<FeedEvent>{ev(Type::Executed, 2, oid(1, 1), Side::Buy, 151, 3, 1)}));
    EXPECT_EQ(responses[0].second.restingQty, 0u);
}

TEST_F(FeedTest, CancelEmitsDeleted) {
    run({add(oid(1, 1), Side::Sell, 151, 3)});
    run({cancel(oid(1, 1))});
    EXPECT_EQ(events, (std::vector<FeedEvent>{ev(Type::Deleted, 2, oid(1, 1), Side::Buy, 0, 0, 0)}));
    EXPECT_EQ(responses[0].second, (ResponseMsg{1, 3, Reject::None, 2, 0}));
}

TEST_F(FeedTest, ReplaceReduceEmitsReduced) {
    run({add(oid(1, 1), Side::Sell, 151, 10)});
    run({replace(oid(1, 1), 151, 4)});
    EXPECT_EQ(events, (std::vector<FeedEvent>{ev(Type::Reduced, 2, oid(1, 1), Side::Buy, 0, 4, 0)}));
    EXPECT_EQ(responses[0].second, (ResponseMsg{1, 4, Reject::None, 2, 4}));
}

TEST_F(FeedTest, ReplaceMoveEmitsDeletedExecutedAdded) {
    run({add(oid(1, 1), Side::Sell, 152, 2), add(oid(2, 1), Side::Buy, 150, 5)});
    run({replace(oid(2, 1), 152, 5)});  // moves up, crosses the resting sell, rests the rest
    EXPECT_EQ(events, (std::vector<FeedEvent>{
                          ev(Type::Deleted, 3, oid(2, 1), Side::Buy, 0, 0, 0),
                          ev(Type::Executed, 4, oid(1, 1), Side::Buy, 152, 2, 1),
                          ev(Type::Added, 5, oid(2, 1), Side::Buy, 152, 3, 0),
                      }));
    EXPECT_EQ(responses[0].second, (ResponseMsg{1, 4, Reject::None, 3, 3}));
    EXPECT_EQ(executions.size(), 2u);
}

TEST_F(FeedTest, RejectedCommandsEmitNothing) {
    run({add(oid(1, 1), Side::Sell, 151, 2), add(oid(2, 1), Side::Buy, 151, 2)});  // order (1,1) fills
    run({add(oid(1, 2), Side::Buy, 999, 1), cancel(oid(1, 77)), replace(oid(1, 1), 150, 1)});
    EXPECT_TRUE(events.empty());
    ASSERT_EQ(responses.size(), 3u);
    EXPECT_EQ(responses[0].second.result, Reject::BadPrice);
    EXPECT_EQ(responses[1].second.result, Reject::UnknownOrder);
    EXPECT_EQ(responses[2].second.result, Reject::UnknownOrder);
}

TEST(FeedBook, RejectsGapAndCrossing) {
    FeedBook book(kCfg);
    EXPECT_TRUE(book.apply(ev(Type::Added, 1, oid(1, 1), Side::Sell, 150, 5, 0)));
    EXPECT_FALSE(book.apply(ev(Type::Added, 3, oid(1, 2), Side::Sell, 151, 5, 0)));  // gap
    FeedBook crossed(kCfg);
    EXPECT_TRUE(crossed.apply(ev(Type::Added, 1, oid(1, 1), Side::Sell, 150, 5, 0)));
    EXPECT_FALSE(crossed.apply(ev(Type::Added, 2, oid(2, 1), Side::Buy, 150, 5, 0)));  // would trade
    FeedBook unknown(kCfg);
    EXPECT_FALSE(unknown.apply(ev(Type::Deleted, 1, oid(9, 9), Side::Buy, 0, 0, 0)));
}

TEST(FeedEquivalence, MillionCommands) {
#ifdef HARBOR_SANITIZED
    constexpr int kCommands = 200'000;
#else
    constexpr int kCommands = 1'000'000;
#endif
    constexpr BookConfig cfg{1'000, 2'000, 1, 4096};
    harbor::testing::TempDir dir;
    EngineOptions o;
    o.dir = dir.path();
    o.maxBatch = 64;
    o.sync = SyncMode::None;
    o.snapshotEvery = 1'000'000'000;
    FeedEngine fe(cfg, o);
    FeedBook feed(cfg);
    harbor::testing::OpGenerator gen(21, 1'500, 100, cfg.minPrice, cfg.maxPrice);
    using Entry = std::tuple<OrderId, Side, Price, Quantity>;
    auto restingOf = [](const auto& book) {
        std::vector<Entry> out;
        book.forEachResting([&](OrderId id, ClientId, Side s, Price p, Quantity q) { out.emplace_back(id, s, p, q); });
        return out;
    };
    std::uint64_t commits = 0;
    std::uint64_t lastDurable = 0;
    bool consistent = true;
    for (int i = 0; i < kCommands && consistent; ++i) {
        fe.submit(gen.next([&](OrderId id) { return fe.book().contains(id); }), Clock::time_point{});
        if (fe.lastDurableSeq() != lastDurable) {  // a batch just committed
            lastDurable = fe.lastDurableSeq();
            fe.drain([&](const FeedEvent& e) { consistent = consistent && feed.apply(e); },
                     [](const ResponseMsg&, ClientId, std::uint64_t) {}, [](const ExecutionMsg&, ClientId) {});
            ASSERT_TRUE(consistent) << "feed event rejected after command " << i;
            ++commits;
            if (fe.pendingCount() == 0) {  // the book holds nothing undurable: compare exactly
                ASSERT_EQ(restingOf(feed.book()), restingOf(fe.book())) << "after command " << i;
            }
        }
    }
    EXPECT_GT(commits, 1'000u);
    EXPECT_EQ(feed.lastSeq(), fe.lastFeedSeq());
}
