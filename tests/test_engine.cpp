#include <gtest/gtest.h>
#include <chrono>
#include <vector>
#include "harbor/engine.hpp"
#include "harbor/fast_book.hpp"
#include "harbor/wal.hpp"
#include "temp_dir.hpp"
using namespace harbor;
using namespace std::chrono_literals;

namespace {

constexpr BookConfig kCfg{100, 200, 1, 64};
using Clock = std::chrono::steady_clock;
const Clock::time_point t0{};

Command add(OrderId id, Side side, Price price, Quantity qty) {
    return Command{Command::Kind::Add, NewOrder{id, 1, side, price, qty, TimeInForce::GTC}, 0, 0, 0};
}

struct Seen {
    std::uint64_t seq;
    Reject result;
    std::vector<Fill> fills;
};

class EngineTest : public ::testing::Test {
protected:
    harbor::testing::TempDir dir;

    EngineOptions options(std::uint32_t maxBatch = 100) {
        EngineOptions o;
        o.dir = dir.path();
        o.maxBatch = maxBatch;
        o.maxDelay = 1000us;
        o.sync = SyncMode::None;
        return o;
    }

    template <class E>
    std::vector<Seen> drainAll(E& engine) {
        std::vector<Seen> out;
        engine.drain([&](const Output& o) {
            out.push_back(Seen{o.seq, o.result, std::vector<Fill>(o.fills.begin(), o.fills.end())});
        });
        return out;
    }
};

}  // namespace

TEST_F(EngineTest, CreatesFirstSegment) {
    Engine<FastBook> engine(kCfg, options());
    WalScan scan = scanWal(dir.path() / walName(0), [](std::uint64_t, const Command&) {});
    EXPECT_EQ(scan.segment, 0u);
    EXPECT_EQ(scan.firstSeq, 1u);
    EXPECT_EQ(engine.lastSeq(), 0u);
    EXPECT_EQ(engine.lastDurableSeq(), 0u);
}

TEST_F(EngineTest, NothingDrainedBeforeCommit) {
    Engine<FastBook> engine(kCfg, options());
    for (OrderId id = 1; id <= 3; ++id) {
        EXPECT_EQ(engine.submit(add(id, Side::Buy, 150, 1), t0), id);
    }
    EXPECT_TRUE(drainAll(engine).empty());
    EXPECT_EQ(engine.lastSeq(), 3u);
    EXPECT_EQ(engine.lastDurableSeq(), 0u);
    EXPECT_EQ(engine.book().orderCount(), 3u);  // applied immediately, just not acknowledged
}

TEST_F(EngineTest, MaxBatchTriggersCommit) {
    Engine<FastBook> engine(kCfg, options(4));
    for (OrderId id = 1; id <= 3; ++id) {
        engine.submit(add(id, Side::Buy, 150, 1), t0);
    }
    EXPECT_EQ(engine.lastDurableSeq(), 0u);
    engine.submit(add(4, Side::Buy, 150, 1), t0);
    EXPECT_EQ(engine.lastDurableSeq(), 4u);
    auto out = drainAll(engine);
    ASSERT_EQ(out.size(), 4u);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(out[i].seq, i + 1);
    }
}

TEST_F(EngineTest, PollCommitsAfterMaxDelay) {
    Engine<FastBook> engine(kCfg, options());
    engine.submit(add(1, Side::Buy, 150, 1), t0);
    engine.submit(add(2, Side::Buy, 150, 1), t0 + 500us);
    engine.poll(t0 + 999us);
    EXPECT_EQ(engine.lastDurableSeq(), 0u);
    EXPECT_TRUE(drainAll(engine).empty());
    engine.poll(t0 + 1000us);  // measured from the first pending command
    EXPECT_EQ(engine.lastDurableSeq(), 2u);
    EXPECT_EQ(drainAll(engine).size(), 2u);
    engine.poll(t0 + 5000us);  // nothing pending: no-op
    EXPECT_EQ(engine.lastDurableSeq(), 2u);
}

TEST_F(EngineTest, OutputsCarryResultsAndFills) {
    Engine<FastBook> engine(kCfg, options());
    engine.submit(add(1, Side::Sell, 150, 5), t0);
    engine.submit(add(2, Side::Buy, 151, 3), t0);
    engine.submit(add(3, Side::Buy, 999, 1), t0);
    engine.submit(Command{Command::Kind::Cancel, {}, 1, 0, 0}, t0);
    engine.commit();
    auto out = drainAll(engine);
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[0].result, Reject::None);
    EXPECT_TRUE(out[0].fills.empty());
    EXPECT_EQ(out[1].result, Reject::None);
    EXPECT_EQ(out[1].fills, (std::vector<Fill>{{2, 1, 150, 3}}));
    EXPECT_EQ(out[2].result, Reject::BadPrice);
    EXPECT_EQ(out[3].result, Reject::None);
    EXPECT_TRUE(out[3].fills.empty());
}

TEST_F(EngineTest, DrainSkipsPendingAfterCommit) {
    Engine<FastBook> engine(kCfg, options());
    engine.submit(add(1, Side::Sell, 150, 5), t0);
    engine.submit(add(2, Side::Buy, 150, 2), t0);
    engine.commit();
    engine.submit(add(3, Side::Buy, 150, 1), t0);  // after the commit: still pending
    auto first = drainAll(engine);
    ASSERT_EQ(first.size(), 2u);
    EXPECT_EQ(first[1].fills, (std::vector<Fill>{{2, 1, 150, 2}}));
    EXPECT_TRUE(drainAll(engine).empty());
    engine.commit();
    auto second = drainAll(engine);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0].seq, 3u);
    EXPECT_EQ(second[0].fills, (std::vector<Fill>{{3, 1, 150, 1}}));
}

TEST_F(EngineTest, LogContainsEveryCommand) {
    Engine<FastBook> engine(kCfg, options());
    engine.submit(add(1, Side::Buy, 150, 1), t0);
    engine.submit(add(2, Side::Buy, 999, 1), t0);  // rejected, still logged
    engine.submit(Command{Command::Kind::Cancel, {}, 77, 0, 0}, t0);
    engine.commit();
    std::vector<std::uint64_t> seqs;
    std::vector<Command::Kind> kinds;
    scanWal(dir.path() / walName(0), [&](std::uint64_t seq, const Command& cmd) {
        seqs.push_back(seq);
        kinds.push_back(cmd.kind);
    });
    EXPECT_EQ(seqs, (std::vector<std::uint64_t>{1, 2, 3}));
    EXPECT_EQ(kinds, (std::vector<Command::Kind>{Command::Kind::Add, Command::Kind::Add,
                                                 Command::Kind::Cancel}));
}

TEST_F(EngineTest, UncommittedCommandsNotInLog) {
    {
        Engine<FastBook> engine(kCfg, options());
        engine.submit(add(1, Side::Buy, 150, 1), t0);
        engine.commit();
        engine.submit(add(2, Side::Buy, 150, 1), t0);  // abandoned with the engine
    }
    WalScan scan = scanWal(dir.path() / walName(0), [](std::uint64_t, const Command&) {});
    EXPECT_EQ(scan.nextSeq, 2u);
}
