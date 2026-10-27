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

// ---- Snapshots through the engine loop, and the feed listener ----

#include <algorithm>
#include <deque>
#include <random>
#include "harbor/engine_loop.hpp"
#include "harbor/feed_listener.hpp"

TEST(EngineLoopSnapshot, SnapshotExcludesPendingCommands) {
    harbor::testing::TempDir dir;
    auto inbound = std::make_unique<InboundRing>();
    auto outbound = std::make_unique<OutboundRing>();
    EngineOptions o;
    o.dir = dir.path();
    o.maxBatch = 1'000;
    o.maxDelay = std::chrono::hours(1);  // nothing commits on its own
    o.sync = SyncMode::None;
    EngineLoop loop(BookConfig{100, 200, 1, 64}, o, *inbound, *outbound);
    auto now = Clock::time_point{};

    auto addOrders = [&](std::uint32_t first, std::uint32_t count) {
        for (std::uint32_t i = first; i < first + count; ++i) {
            InboundItem item{};
            item.kind = InboundItem::Kind::Command;
            item.cmd = add(oid(1, i), Side::Buy, 100 + i, 1);
            ASSERT_TRUE(inbound->tryPush(item));
        }
    };
    auto snapshot = [&](std::uint64_t& lastEventSeq, wire::SnapshotHeaderMsg& header, std::size_t& orders) {
        InboundItem item{};
        item.kind = InboundItem::Kind::Snapshot;
        item.origin = 99;
        ASSERT_TRUE(inbound->tryPush(item));
        loop.runOnce(now);
        OutboundItem out{};
        bool sawHeader = false, sawEnd = false;
        orders = 0;
        while (outbound->tryPop(out)) {
            if (out.kind == OutboundItem::Kind::Feed) {
                EXPECT_FALSE(sawHeader) << "every feed event must precede the snapshot header";
                lastEventSeq = out.event.seq;
            } else if (out.kind == OutboundItem::Kind::SnapshotHeader) {
                sawHeader = true;
                header = out.snapHeader;
            } else if (out.kind == OutboundItem::Kind::SnapshotOrder) {
                ++orders;
            } else if (out.kind == OutboundItem::Kind::SnapshotEnd) {
                sawEnd = true;
                EXPECT_EQ(out.origin, 99u);
            }
        }
        EXPECT_TRUE(sawHeader && sawEnd);
    };

    addOrders(0, 5);
    loop.runOnce(now);
    OutboundItem out{};
    EXPECT_FALSE(outbound->tryPop(out)) << "pending commands must not produce output";
    std::uint64_t lastEvent = 0;
    wire::SnapshotHeaderMsg header{};
    std::size_t orders = 0;
    snapshot(lastEvent, header, orders);  // forces the commit, then describes exactly that state
    EXPECT_EQ(lastEvent, 5u);
    EXPECT_EQ(header, (wire::SnapshotHeaderMsg{5, 5}));
    EXPECT_EQ(orders, 5u);

    addOrders(5, 3);
    loop.runOnce(now);
    snapshot(lastEvent, header, orders);
    EXPECT_EQ(header, (wire::SnapshotHeaderMsg{8, 8}));
    EXPECT_EQ(lastEvent, 8u);
}

namespace {

// Packets between publisher and listener, with seeded loss, duplication, and reordering.
class LossyChannel {
public:
    explicit LossyChannel(unsigned seed) : rng_(seed) {}
    bool lossy = true;
    std::uint64_t dropped = 0;

    void send(std::span<const std::uint8_t> packet) {
        std::vector<std::uint8_t> p(packet.begin(), packet.end());
        if (lossy) {
            unsigned roll = rng_() % 100;
            if (roll < 2) {
                ++dropped;
                return;
            }
            if (roll < 3) {
                queue_.push_back(p);  // duplicate
            }
            if (roll < 4 && !queue_.empty()) {
                std::swap(p, queue_.back());  // swap with the previous packet
            }
        }
        queue_.push_back(std::move(p));
    }

    template <class F>
    void deliver(F&& f) {
        while (!queue_.empty()) {
            auto p = std::move(queue_.front());
            queue_.pop_front();
            f(std::span<const std::uint8_t>(p));
        }
    }

private:
    std::mt19937 rng_;
    std::deque<std::vector<std::uint8_t>> queue_;
};

template <class Book>
std::vector<std::tuple<OrderId, Side, Price, Quantity>> restingIn(const Book& book) {
    std::vector<std::tuple<OrderId, Side, Price, Quantity>> out;
    book.forEachResting([&](OrderId id, ClientId, Side s, Price p, Quantity q) { out.emplace_back(id, s, p, q); });
    return out;
}

}  // namespace

TEST(FeedListener, ConvergesUnderLoss) {
#ifdef HARBOR_SANITIZED
    constexpr int kCommands = 60'000;
#else
    constexpr int kCommands = 300'000;
#endif
    constexpr BookConfig cfg{1'000, 2'000, 1, 4096};
    for (unsigned seed : {1u, 2u, 3u}) {
        SCOPED_TRACE("seed " + std::to_string(seed));
        harbor::testing::TempDir dir;
        EngineOptions o;
        o.dir = dir.path();
        o.maxBatch = 64;
        o.sync = SyncMode::None;
        o.snapshotEvery = 1'000'000'000;
        FeedEngine fe(cfg, o);
        LossyChannel channel(seed);
        wire::FeedPacketizer packetizer(7);
        auto flush = [&] {
            if (!packetizer.empty()) {
                channel.send(packetizer.packet());
                packetizer.clear();
            }
        };
        auto publishReleased = [&] {
            fe.drain(
                [&](const FeedEvent& e) {
                    if (!packetizer.add(e)) {
                        flush();
                        packetizer.add(e);
                    }
                },
                [](const ResponseMsg&, ClientId, std::uint64_t) {}, [](const ExecutionMsg&, ClientId) {});
            flush();
        };
        // The snapshot service: commit, publish everything released, then describe that state.
        FeedListener listener(cfg, [&](std::uint64_t& feedSeq, std::vector<wire::SnapshotOrderMsg>& orders) {
            fe.commit();
            publishReleased();
            feedSeq = fe.lastFeedSeq();
            orders.clear();
            fe.book().forEachResting([&](OrderId id, ClientId, Side s, Price p, Quantity q) {
                orders.push_back(wire::SnapshotOrderMsg{id, s, p, q});
            });
            return true;
        });
        harbor::testing::OpGenerator gen(seed, 1'500, 100, cfg.minPrice, cfg.maxPrice);
        for (int i = 0; i < kCommands; ++i) {
            fe.submit(gen.next([&](OrderId id) { return fe.book().contains(id); }), Clock::time_point{});
            publishReleased();
            channel.deliver([&](std::span<const std::uint8_t> p) { listener.onPacket(p); });
        }
        fe.commit();
        publishReleased();
        channel.lossy = false;
        // A lost final packet is only noticed when the next one arrives: send a
        // final event without loss so the listener can detect and heal any tail gap.
        fe.submit(Command{Command::Kind::Add, NewOrder{1ULL << 40, 1, Side::Buy, 1'000, 1, TimeInForce::GTC}, 0, 0, 0},
                  Clock::time_point{});
        fe.commit();
        publishReleased();
        channel.deliver([&](std::span<const std::uint8_t> p) { listener.onPacket(p); });

        EXPECT_GT(channel.dropped, 0u);
        EXPECT_GT(listener.gaps(), 0u);
        EXPECT_TRUE(listener.synced());
        EXPECT_EQ(listener.book().lastSeq(), fe.lastFeedSeq());
        EXPECT_EQ(restingIn(listener.book().book()), restingIn(fe.book()));
    }
}

TEST(FeedListener, SessionChangeResyncs) {
    constexpr BookConfig cfg{100, 200, 1, 64};
    int fetches = 0;
    FeedListener listener(cfg, [&](std::uint64_t& feedSeq, std::vector<wire::SnapshotOrderMsg>& orders) {
        ++fetches;
        feedSeq = fetches == 1 ? 0 : 10;
        orders.clear();
        return true;
    });
    wire::FeedPacketizer first(1);
    first.add(FeedEvent{FeedEvent::Type::Added, 1, oid(1, 1), Side::Buy, 150, 1, 0});
    listener.onPacket(first.packet());
    EXPECT_TRUE(listener.synced());
    EXPECT_EQ(listener.snapshots(), 1u);
    wire::FeedPacketizer restarted(2);  // a restarted server starts a new session, seqs from 11 here
    restarted.add(FeedEvent{FeedEvent::Type::Added, 11, oid(1, 2), Side::Buy, 151, 1, 0});
    listener.onPacket(restarted.packet());
    EXPECT_EQ(listener.snapshots(), 2u);
    EXPECT_TRUE(listener.synced());
    EXPECT_EQ(listener.book().lastSeq(), 11u);
    EXPECT_FALSE(listener.book().book().contains(oid(1, 1)));  // the old session's state is gone
}

TEST(FeedSession, IncrementsAcrossRestarts) {
    harbor::testing::TempDir dir;
    EXPECT_EQ(nextFeedSession(dir.path(), SyncMode::None), 1u);
    EXPECT_EQ(nextFeedSession(dir.path(), SyncMode::None), 2u);
    EXPECT_EQ(nextFeedSession(dir.path(), SyncMode::None), 3u);
}
