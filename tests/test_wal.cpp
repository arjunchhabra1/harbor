#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include "harbor/command.hpp"
#include "harbor/file_io.hpp"
#include "harbor/wal.hpp"
#include "temp_dir.hpp"
using namespace harbor;

namespace {

Command cancelOf(OrderId id) { return Command{Command::Kind::Cancel, {}, id, 0, 0}; }

class WalTest : public ::testing::Test {
protected:
    harbor::testing::TempDir dir;
    std::filesystem::path file = dir.path() / walName(3);

    // Header for segment 3 starting at seq 101, then n records seq 101..100+n.
    void writeLog(int n) {
        File f = File::create(file);
        writeWalHeader(f, 3, 101);
        std::vector<std::uint8_t> buf(kRecordSize);
        for (int i = 0; i < n; ++i) {
            encodeRecord(101 + static_cast<std::uint64_t>(i), cancelOf(static_cast<OrderId>(i)), buf.data());
            f.writeAll(buf.data(), buf.size());
        }
        f.sync(SyncMode::None);
    }

    void flipByte(std::uintmax_t offset) {
        std::fstream s(file, std::ios::in | std::ios::out | std::ios::binary);
        s.seekg(static_cast<std::streamoff>(offset));
        char c = 0;
        s.get(c);
        s.seekp(static_cast<std::streamoff>(offset));
        s.put(static_cast<char>(c ^ 0x5A));
    }

    void appendBytes(std::size_t n) {
        std::ofstream s(file, std::ios::app | std::ios::binary);
        for (std::size_t i = 0; i < n; ++i) {
            s.put(static_cast<char>(0xAB));
        }
    }

    std::vector<std::uint64_t> seqs;
    WalScan scan() {
        seqs.clear();
        return scanWal(file, [&](std::uint64_t seq, const Command& cmd) {
            EXPECT_EQ(cmd.kind, Command::Kind::Cancel);
            EXPECT_EQ(cmd.id, seq - 101);
            seqs.push_back(seq);
        });
    }
};

std::uintmax_t recordOffset(int i) { return kWalHeaderSize + static_cast<std::uintmax_t>(i) * kRecordSize; }

}  // namespace

TEST(WalNames, ArePadded) {
    EXPECT_EQ(walName(42), "wal-000000000042.log");
    EXPECT_EQ(snapName(7), "snap-000000000007.bin");
}

TEST_F(WalTest, RoundTrip) {
    writeLog(5);
    WalScan s = scan();
    EXPECT_EQ(s.segment, 3u);
    EXPECT_EQ(s.firstSeq, 101u);
    EXPECT_EQ(s.nextSeq, 106u);
    EXPECT_EQ(s.validBytes, recordOffset(5));
    EXPECT_FALSE(s.tornTail);
    EXPECT_EQ(seqs, (std::vector<std::uint64_t>{101, 102, 103, 104, 105}));
}

TEST_F(WalTest, EmptySegment) {
    writeLog(0);
    WalScan s = scan();
    EXPECT_EQ(s.nextSeq, 101u);
    EXPECT_EQ(s.validBytes, kWalHeaderSize);
    EXPECT_FALSE(s.tornTail);
}

TEST_F(WalTest, PartialLastRecordTruncated) {
    writeLog(5);
    appendBytes(20);
    WalScan s = scan();
    EXPECT_TRUE(s.tornTail);
    EXPECT_EQ(s.nextSeq, 106u);
    EXPECT_EQ(s.validBytes, recordOffset(5));
}

TEST_F(WalTest, BadRecordsToEndTruncated) {
    writeLog(5);
    flipByte(recordOffset(3) + 10);
    flipByte(recordOffset(4) + 30);
    WalScan s = scan();
    EXPECT_TRUE(s.tornTail);
    EXPECT_EQ(s.nextSeq, 104u);
    EXPECT_EQ(s.validBytes, recordOffset(3));
    EXPECT_EQ(seqs.size(), 3u);
}

TEST_F(WalTest, BadThenGoodFails) {
    writeLog(5);
    flipByte(recordOffset(1) + 5);
    EXPECT_THROW(scan(), std::runtime_error);
}

TEST_F(WalTest, SeqGapFails) {
    {
        File f = File::create(file);
        writeWalHeader(f, 3, 101);
        std::vector<std::uint8_t> buf(kRecordSize);
        encodeRecord(101, cancelOf(0), buf.data());
        f.writeAll(buf.data(), buf.size());
        encodeRecord(103, cancelOf(2), buf.data());
        f.writeAll(buf.data(), buf.size());
    }
    EXPECT_THROW(scan(), std::runtime_error);
}

TEST_F(WalTest, BadHeaderFails) {
    writeLog(2);
    flipByte(9);
    EXPECT_THROW(scan(), std::runtime_error);
}

TEST_F(WalTest, ShortHeaderFails) {
    { std::ofstream s(file, std::ios::binary); s << "HWAL"; }
    EXPECT_THROW(scan(), std::runtime_error);
}

TEST_F(WalTest, CreateRefusesExistingFile) {
    writeLog(0);
    EXPECT_THROW(File::create(file), std::runtime_error);
}

TEST_F(WalTest, AppendAfterTruncate) {
    writeLog(3);
    appendBytes(7);
    WalScan s = scan();
    truncateFile(file, s.validBytes);
    {
        File f = File::openAppend(file);
        std::vector<std::uint8_t> buf(kRecordSize);
        encodeRecord(104, cancelOf(3), buf.data());
        f.writeAll(buf.data(), buf.size());
    }
    s = scan();
    EXPECT_FALSE(s.tornTail);
    EXPECT_EQ(s.nextSeq, 105u);
}
