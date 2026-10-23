#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include "crash_harness.hpp"
#include "harbor/file_io.hpp"
#include "harbor/wal.hpp"
#include "temp_dir.hpp"
using namespace harbor;
using harbor::testing::CrashResult;
using harbor::testing::TempDir;

TEST(Crash, KillAtRandomPoints) {
    std::uint64_t snapshotsSeen = 0;
    for (std::uint32_t trial = 0; trial < 200; ++trial) {
        TempDir dir;
        std::uint64_t killAfter = (trial * 7919) % 12'000;   // 0 to 12,000 durable commands
        std::uint32_t extra = (trial * 104'729) % 3'000;     // then 0 to 3 ms more
        CrashResult r = harbor::testing::runCrashTrial(dir.path(), trial, killAfter, extra);
        ASSERT_TRUE(r.error.empty()) << "trial " << trial << ": " << r.error;
        ASSERT_GE(r.recoveredDurable, r.reportedDurable) << "trial " << trial << ": acknowledged commands lost";
        ASSERT_TRUE(r.stateMatches) << "trial " << trial << " recovered " << r.recoveredDurable;
        snapshotsSeen += r.recoveredDurable >= 5'000 ? 1 : 0;
    }
    EXPECT_GT(snapshotsSeen, 0u) << "no trial ran long enough to rotate a snapshot";
}

namespace {

// 1,000 committed commands in segment 0, no snapshot.
void writeLog(const std::filesystem::path& dir) {
    EngineOptions o = harbor::testing::crashOptions(dir);
    o.snapshotEvery = 1'000'000'000;
    Engine<FastBook> engine(harbor::testing::kCrashCfg, o);
    auto gen = harbor::testing::crashGenerator(11);
    std::chrono::steady_clock::time_point now{};
    for (int i = 0; i < 1'000; ++i) {
        engine.submit(gen.next([&](OrderId id) { return engine.book().contains(id); }), now);
        engine.drain([](const Output&) {});
    }
    engine.commit();
}

}  // namespace

TEST(TornWrite, EveryOffsetOfLastRecords) {
    TempDir original;
    writeLog(original.path());
    const auto log = walName(0);
    const std::uintmax_t size = std::filesystem::file_size(original.path() / log);
    ASSERT_EQ(size, kWalHeaderSize + 1'000 * kRecordSize);
    for (std::uintmax_t offset = size - 3 * kRecordSize; offset < size; ++offset) {
        TempDir copy;
        std::filesystem::copy(original.path(), copy.path(), std::filesystem::copy_options::recursive);
        truncateFile(copy.path() / log, offset);
        CrashResult r;
        harbor::testing::checkRecovered(copy.path(), 11, r);
        ASSERT_TRUE(r.error.empty()) << "offset " << offset << ": " << r.error;
        EXPECT_EQ(r.recoveredDurable, (offset - kWalHeaderSize) / kRecordSize) << offset;
        EXPECT_TRUE(r.stateMatches) << offset;
    }
}

TEST(TornWrite, FlippedByteMidLogFails) {
    TempDir original;
    writeLog(original.path());
    for (std::uintmax_t record = 0; record < 5; ++record) {
        TempDir copy;
        std::filesystem::copy(original.path(), copy.path(), std::filesystem::copy_options::recursive);
        {
            std::fstream s(copy.path() / walName(0), std::ios::in | std::ios::out | std::ios::binary);
            auto at = static_cast<std::streamoff>(kWalHeaderSize + record * kRecordSize + 17);
            s.seekg(at);
            char c = 0;
            s.get(c);
            s.seekp(at);
            s.put(static_cast<char>(c ^ 0x5A));
        }
        EXPECT_THROW((Engine<FastBook>(harbor::testing::kCrashCfg, harbor::testing::crashOptions(copy.path()))),
                     std::runtime_error)
            << "record " << record;
    }
}

namespace {

// 1,500 commands with maxBatch 1,024: batch A = seqs 1-1,024, batch B = 1,025-1,500.
void writeTwoBatchLog(const std::filesystem::path& dir) {
    EngineOptions o = harbor::testing::crashOptions(dir);
    o.maxBatch = 1'024;
    o.snapshotEvery = 1'000'000'000;
    Engine<FastBook> engine(harbor::testing::kCrashCfg, o);
    auto gen = harbor::testing::crashGenerator(13);
    std::chrono::steady_clock::time_point now{};
    for (int i = 0; i < 1'500; ++i) {
        engine.submit(gen.next([&](OrderId id) { return engine.book().contains(id); }), now);
        engine.drain([](const Output&) {});
    }
    engine.commit();
}

// What power loss during a sync can leave: a page-sized run of zeros.
void zeroBytes(const std::filesystem::path& file, std::uintmax_t offset, std::uintmax_t count) {
    std::fstream s(file, std::ios::in | std::ios::out | std::ios::binary);
    s.seekp(static_cast<std::streamoff>(offset));
    for (std::uintmax_t i = 0; i < count; ++i) {
        s.put('\0');
    }
}

}  // namespace

TEST(TornWrite, ZeroedPageInsideFinalBatchRecovers) {
    TempDir dir;
    writeTwoBatchLog(dir.path());
    std::uintmax_t holeStart = kWalHeaderSize + 1'100 * kRecordSize;  // inside batch B
    zeroBytes(dir.path() / walName(0), holeStart, 4'096);
    CrashResult r;
    harbor::testing::checkRecovered(dir.path(), 13, r);
    ASSERT_TRUE(r.error.empty()) << r.error;
    EXPECT_EQ(r.recoveredDurable, 1'100u);  // records 1-1,100 intact; the rest of B was never acknowledged
    EXPECT_TRUE(r.stateMatches);
}

TEST(TornWrite, ZeroedPageInsideAcknowledgedBatchFails) {
    TempDir dir;
    writeTwoBatchLog(dir.path());
    zeroBytes(dir.path() / walName(0), kWalHeaderSize + 500 * kRecordSize, 4'096);  // inside batch A
    CrashResult r;
    harbor::testing::checkRecovered(dir.path(), 13, r);
    EXPECT_FALSE(r.error.empty());
}
