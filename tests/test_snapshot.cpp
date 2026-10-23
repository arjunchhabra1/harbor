#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <tuple>
#include <vector>
#include "harbor/command.hpp"
#include "harbor/fast_book.hpp"
#include "harbor/file_io.hpp"
#include "harbor/reference_book.hpp"
#include "harbor/snapshot.hpp"
#include "op_generator.hpp"
#include "temp_dir.hpp"
using namespace harbor;

namespace {

constexpr BookConfig kCfg{1'000, 2'000, 1, 4096};

using Entry = std::tuple<OrderId, ClientId, Side, Price, Quantity>;

template <class Book>
std::vector<Entry> resting(const Book& book) {
    std::vector<Entry> out;
    book.forEachResting([&](OrderId id, ClientId c, Side s, Price p, Quantity q) {
        out.emplace_back(id, c, s, p, q);
    });
    return out;
}

FastBook generatedBook(int ops) {
    FastBook book{kCfg};
    harbor::testing::OpGenerator gen(7, 1'500, 100, kCfg.minPrice, kCfg.maxPrice);
    std::vector<Fill> fills;
    for (int i = 0; i < ops; ++i) {
        applyCommand(book, gen.next([&](OrderId id) { return book.contains(id); }), fills);
    }
    return book;
}

void flipByte(const std::filesystem::path& file, std::uintmax_t offset) {
    std::fstream s(file, std::ios::in | std::ios::out | std::ios::binary);
    s.seekg(static_cast<std::streamoff>(offset));
    char c = 0;
    s.get(c);
    s.seekp(static_cast<std::streamoff>(offset));
    s.put(static_cast<char>(c ^ 0x5A));
}

}  // namespace

TEST(Snapshot, RoundTripBothBooks) {
    harbor::testing::TempDir dir;
    FastBook original = generatedBook(20'000);
    ASSERT_GT(original.orderCount(), 100u);
    auto file = dir.path() / snapName(4);
    writeSnapshotFile(file, captureSnapshot(original, kCfg, 4, 20'000), SyncMode::None);

    Snapshot snap = readSnapshotFile(file);
    EXPECT_EQ(snap.segment, 4u);
    EXPECT_EQ(snap.lastSeq, 20'000u);
    EXPECT_EQ(snap.cfg, kCfg);
    EXPECT_EQ(snap.orders.size(), original.orderCount());

    FastBook fast{kCfg};
    ReferenceBook ref{kCfg};
    restoreSnapshot(fast, snap);
    restoreSnapshot(ref, snap);
    for (const auto& [name, got] : {std::pair{"fast", resting(fast)}, std::pair{"ref", resting(ref)}}) {
        EXPECT_EQ(got, resting(original)) << name;
    }
    EXPECT_EQ(fast.bestBid(), original.bestBid());
    EXPECT_EQ(fast.bestAsk(), original.bestAsk());
    EXPECT_EQ(ref.bestBid(), original.bestBid());
    EXPECT_EQ(ref.bestAsk(), original.bestAsk());
    std::string e;
    EXPECT_TRUE(fast.checkInvariants(e)) << e;
}

TEST(Snapshot, EmptyBook) {
    harbor::testing::TempDir dir;
    FastBook empty{kCfg};
    auto file = dir.path() / snapName(0);
    writeSnapshotFile(file, captureSnapshot(empty, kCfg, 0, 0), SyncMode::None);
    Snapshot snap = readSnapshotFile(file);
    EXPECT_TRUE(snap.orders.empty());
    ReferenceBook ref{kCfg};
    restoreSnapshot(ref, snap);
    EXPECT_EQ(ref.orderCount(), 0u);
}

TEST(Snapshot, FlippedByteFails) {
    harbor::testing::TempDir dir;
    FastBook book = generatedBook(2'000);
    ASSERT_GT(book.orderCount(), 0u);
    auto file = dir.path() / snapName(1);
    writeSnapshotFile(file, captureSnapshot(book, kCfg, 1, 2'000), SyncMode::None);
    auto size = std::filesystem::file_size(file);
    for (std::uintmax_t offset : {std::uintmax_t{10}, std::uintmax_t{56 + 9}, size - 2}) {
        auto copy = dir.path() / "copy.bin";
        std::filesystem::copy_file(file, copy, std::filesystem::copy_options::overwrite_existing);
        flipByte(copy, offset);
        EXPECT_THROW(readSnapshotFile(copy), std::runtime_error) << offset;
    }
}

TEST(Snapshot, TruncatedFails) {
    harbor::testing::TempDir dir;
    FastBook book = generatedBook(2'000);
    auto file = dir.path() / snapName(1);
    writeSnapshotFile(file, captureSnapshot(book, kCfg, 1, 2'000), SyncMode::None);
    truncateFile(file, std::filesystem::file_size(file) - 32);
    EXPECT_THROW(readSnapshotFile(file), std::runtime_error);
}

TEST(Snapshot, RestoreRejectsCrossingEntries) {
    Snapshot snap{0, 0, kCfg, {{1, 1, Side::Buy, 1'500, 5}, {2, 1, Side::Sell, 1'400, 5}}};
    FastBook book{kCfg};
    EXPECT_THROW(restoreSnapshot(book, snap), std::runtime_error);
}
