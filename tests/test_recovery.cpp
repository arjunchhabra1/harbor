#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <tuple>
#include <vector>
#include "harbor/command.hpp"
#include "harbor/engine.hpp"
#include "harbor/fast_book.hpp"
#include "harbor/reference_book.hpp"
#include "op_generator.hpp"
#include "temp_dir.hpp"
using namespace harbor;
using namespace std::chrono_literals;

namespace {

constexpr BookConfig kCfg{1'000, 2'000, 1, 4096};
using Clock = std::chrono::steady_clock;
using Entry = std::tuple<OrderId, ClientId, Side, Price, Quantity>;

template <class Book>
std::vector<Entry> resting(const Book& book) {
    std::vector<Entry> out;
    book.forEachResting([&](OrderId id, ClientId c, Side s, Price p, Quantity q) {
        out.emplace_back(id, c, s, p, q);
    });
    return out;
}

harbor::testing::OpGenerator generator(std::uint64_t seed) {
    return harbor::testing::OpGenerator(seed, 1'500, 100, kCfg.minPrice, kCfg.maxPrice);
}

// The book after applying the first `count` commands of the seeded stream.
ReferenceBook referenceAt(std::uint64_t seed, std::uint64_t count) {
    ReferenceBook ref{kCfg};
    auto gen = generator(seed);
    std::vector<Fill> fills;
    for (std::uint64_t i = 0; i < count; ++i) {
        applyCommand(ref, gen.next([&](OrderId id) { return ref.contains(id); }), fills);
    }
    return ref;
}

void expectMatches(const Engine<FastBook>& engine, std::uint64_t seed) {
    ReferenceBook ref = referenceAt(seed, engine.lastDurableSeq());
    EXPECT_EQ(resting(engine.book()), resting(ref));
    EXPECT_EQ(engine.book().bestBid(), ref.bestBid());
    EXPECT_EQ(engine.book().bestAsk(), ref.bestAsk());
    std::string e;
    EXPECT_TRUE(engine.book().checkInvariants(e)) << e;
}

std::vector<std::string> filesIn(const std::filesystem::path& dir) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

class RecoveryTest : public ::testing::Test {
protected:
    harbor::testing::TempDir dir;

    EngineOptions options(std::uint64_t snapshotEvery = 1'000'000'000, std::uint32_t maxBatch = 64) {
        EngineOptions o;
        o.dir = dir.path();
        o.maxBatch = maxBatch;
        o.snapshotEvery = snapshotEvery;
        o.sync = SyncMode::None;
        return o;
    }

    // Submits `count` commands from the seeded stream, draining as it goes.
    void drive(Engine<FastBook>& engine, std::uint64_t seed, std::uint64_t count) {
        auto gen = generator(seed);
        Clock::time_point now{};
        for (std::uint64_t i = 0; i < count; ++i) {
            now += 1us;
            engine.submit(gen.next([&](OrderId id) { return engine.book().contains(id); }), now);
            engine.drain([](const Output&) {});
        }
    }
};

}  // namespace

TEST_F(RecoveryTest, RecoverLogOnly) {
    {
        Engine<FastBook> engine(kCfg, options());
        drive(engine, 1, 50'000);
        engine.commit();
    }
    Engine<FastBook> recovered(kCfg, options());
    EXPECT_EQ(recovered.lastDurableSeq(), 50'000u);
    EXPECT_EQ(recovered.lastSeq(), 50'000u);
    expectMatches(recovered, 1);
}

TEST_F(RecoveryTest, RecoverAfterSnapshots) {
    {
        Engine<FastBook> engine(kCfg, options(7'000));
        drive(engine, 2, 50'000);
        engine.commit();
    }
    // 50,000 commands with a snapshot at least every 7,000: only the newest
    // snapshot and its segment remain.
    auto names = filesIn(dir.path());
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0].rfind("snap-", 0), 0u);
    EXPECT_EQ(names[1].rfind("wal-", 0), 0u);
    EXPECT_EQ(names[0].substr(5, 12), names[1].substr(4, 12));

    Engine<FastBook> recovered(kCfg, options(7'000));
    EXPECT_EQ(recovered.lastDurableSeq(), 50'000u);
    expectMatches(recovered, 2);
}

TEST_F(RecoveryTest, UncommittedCommandsAreLost) {
    {
        Engine<FastBook> engine(kCfg, options(1'000'000'000, 100));
        drive(engine, 3, 10);
        EXPECT_EQ(engine.lastDurableSeq(), 0u);
    }
    Engine<FastBook> recovered(kCfg, options());
    EXPECT_EQ(recovered.lastDurableSeq(), 0u);
    EXPECT_EQ(recovered.book().orderCount(), 0u);
}

TEST_F(RecoveryTest, ContinuesSeqAfterRecovery) {
    {
        Engine<FastBook> engine(kCfg, options(400));
        drive(engine, 4, 1'000);
        engine.commit();
    }
    constexpr OrderId kNewId = 1ULL << 40;
    {
        Engine<FastBook> engine(kCfg, options(400));
        Command add{Command::Kind::Add, NewOrder{kNewId, 9, Side::Buy, 1'001, 3, TimeInForce::GTC}, 0, 0, 0};
        EXPECT_EQ(engine.submit(add, Clock::time_point{}), 1'001u);
        engine.commit();
    }
    Engine<FastBook> recovered(kCfg, options(400));
    EXPECT_EQ(recovered.lastDurableSeq(), 1'001u);
    EXPECT_TRUE(recovered.book().contains(kNewId));
}

TEST_F(RecoveryTest, RecoverHeaderOnlySegment) {
    {
        Engine<FastBook> engine(kCfg, options(64, 64));
        drive(engine, 5, 64);  // the 64th submit commits, which takes a snapshot
    }
    WalScan scan = scanWal(dir.path() / walName(1), [](std::uint64_t, const Command&) {});
    EXPECT_EQ(scan.validBytes, kWalHeaderSize);
    Engine<FastBook> recovered(kCfg, options(64, 64));
    EXPECT_EQ(recovered.lastDurableSeq(), 64u);
    expectMatches(recovered, 5);
}

TEST_F(RecoveryTest, RecoverTornSegmentHeader) {
    {
        Engine<FastBook> engine(kCfg, options(64, 64));
        drive(engine, 6, 64);
    }
    truncateFile(dir.path() / walName(1), 10);  // crash while writing the new segment's header
    {
        Engine<FastBook> recovered(kCfg, options(64, 64));
        EXPECT_EQ(recovered.lastDurableSeq(), 64u);
        expectMatches(recovered, 6);
    }
    Engine<FastBook> again(kCfg, options(64, 64));  // the recreated segment is valid
    EXPECT_EQ(again.lastDurableSeq(), 64u);
}

TEST_F(RecoveryTest, CrashAfterEachSnapshotStep) {
    for (int step = 1; step <= 4; ++step) {
        SCOPED_TRACE("step " + std::to_string(step));
        harbor::testing::TempDir stepDir;
        EngineOptions o = options(1'000, 100);
        o.dir = stepDir.path();
        o.afterSnapshotStep = [step](int s) {
            if (s == step) {
                throw std::runtime_error("simulated crash");
            }
        };
        std::uint64_t durable = 0;
        {
            Engine<FastBook> engine(kCfg, o);
            EXPECT_THROW(drive(engine, 7, 5'000), std::runtime_error);
            durable = engine.lastDurableSeq();
            EXPECT_EQ(durable, 1'000u);  // the commit that triggered the snapshot is durable
        }
        o.afterSnapshotStep = nullptr;
        {
            Engine<FastBook> recovered(kCfg, o);
            EXPECT_EQ(recovered.lastDurableSeq(), durable);
            expectMatches(recovered, 7);
        }
        for (const auto& name : filesIn(stepDir.path())) {
            EXPECT_EQ(name.find(".tmp"), std::string::npos) << name;
        }
        Engine<FastBook> again(kCfg, o);  // recovery left a consistent directory
        EXPECT_EQ(again.lastDurableSeq(), durable);
    }
}

TEST_F(RecoveryTest, ConfigMismatchThrows) {
    {
        Engine<FastBook> engine(kCfg, options(100));
        drive(engine, 8, 200);
        engine.commit();
    }
    BookConfig other = kCfg;
    other.tick = 2;
    EXPECT_THROW((Engine<FastBook>(other, options(100))), std::invalid_argument);
}

TEST_F(RecoveryTest, CorruptSnapshotThrows) {
    {
        Engine<FastBook> engine(kCfg, options(100));
        drive(engine, 9, 500);
        engine.commit();
    }
    std::filesystem::path snap;
    for (const auto& entry : std::filesystem::directory_iterator(dir.path())) {
        if (entry.path().filename().string().rfind("snap-", 0) == 0) {
            snap = entry.path();
        }
    }
    ASSERT_FALSE(snap.empty());
    {
        std::fstream s(snap, std::ios::in | std::ios::out | std::ios::binary);
        s.seekp(60);
        s.put('\x7f');
    }
    EXPECT_THROW((Engine<FastBook>(kCfg, options(100))), std::runtime_error);
}
