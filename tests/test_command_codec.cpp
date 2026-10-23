#include <gtest/gtest.h>
#include <array>
#include <limits>
#include "harbor/bytes.hpp"
#include "harbor/command.hpp"
#include "harbor/crc32c.hpp"
using namespace harbor;

namespace {

using Record = std::array<std::uint8_t, kRecordSize>;

Record encode(std::uint64_t seq, const Command& cmd, std::uint64_t batchFirst = 0) {
    Record r{};
    encodeRecord(seq, batchFirst == 0 ? seq : batchFirst, cmd, r.data());
    return r;
}

bool decode(const Record& r, std::uint64_t& seq, Command& cmd) {
    std::uint64_t batchFirst = 0;
    return decodeRecord(r.data(), seq, batchFirst, cmd);
}

}  // namespace

TEST(CommandCodec, RoundTripsEveryKind) {
    Command add{Command::Kind::Add,
                NewOrder{0, 7, Side::Sell, std::numeric_limits<Price>::min(), 42, TimeInForce::IOC},
                0, 0, 0};
    Command cancel{Command::Kind::Cancel, {}, 99, 0, 0};
    Command replace{Command::Kind::Replace, {}, 123456789012345ULL, -250, 17};

    std::uint64_t seq = 0;
    Command out{};
    ASSERT_TRUE(decode(encode(1, add), seq, out));
    EXPECT_EQ(seq, 1u);
    EXPECT_EQ(out.kind, Command::Kind::Add);
    EXPECT_EQ(out.order.id, 0u);
    EXPECT_EQ(out.order.client, 7u);
    EXPECT_EQ(out.order.side, Side::Sell);
    EXPECT_EQ(out.order.price, std::numeric_limits<Price>::min());
    EXPECT_EQ(out.order.qty, 42u);
    EXPECT_EQ(out.order.tif, TimeInForce::IOC);

    ASSERT_TRUE(decode(encode(2, cancel), seq, out));
    EXPECT_EQ(seq, 2u);
    EXPECT_EQ(out.kind, Command::Kind::Cancel);
    EXPECT_EQ(out.id, 99u);

    ASSERT_TRUE(decode(encode(UINT64_MAX, replace), seq, out));
    EXPECT_EQ(seq, UINT64_MAX);
    EXPECT_EQ(out.kind, Command::Kind::Replace);
    EXPECT_EQ(out.id, 123456789012345ULL);
    EXPECT_EQ(out.price, -250);
    EXPECT_EQ(out.qty, 17u);
}

TEST(CommandCodec, ReservedByteZero) {
    Command add{Command::Kind::Add, NewOrder{~0ULL, ~0U, Side::Sell, -1, ~0U, TimeInForce::IOC},
                0, 0, 0};
    Record r = encode(~0ULL, add);
    EXPECT_EQ(r[11], 0);
}

TEST(CommandCodec, BatchFirstRoundTrips) {
    Record r = encode(500, Command{Command::Kind::Cancel, {}, 3, 0, 0}, 449);
    std::uint64_t seq = 0, batchFirst = 0;
    Command out{};
    ASSERT_TRUE(decodeRecord(r.data(), seq, batchFirst, out));
    EXPECT_EQ(seq, 500u);
    EXPECT_EQ(batchFirst, 449u);
}

TEST(CommandCodec, BatchFirstAfterSeqFails) {
    Record r = encode(500, Command{Command::Kind::Cancel, {}, 3, 0, 0}, 501);
    std::uint64_t seq = 0, batchFirst = 0;
    Command out{};
    EXPECT_FALSE(decodeRecord(r.data(), seq, batchFirst, out));
}

TEST(CommandCodec, CorruptByteFailsCrc) {
    Record r = encode(5, Command{Command::Kind::Cancel, {}, 3, 0, 0});
    r[20] ^= 0x01;
    std::uint64_t seq = 0;
    Command out{};
    EXPECT_FALSE(decode(r, seq, out));
}

TEST(CommandCodec, BadKindFails) {
    Record r = encode(5, Command{Command::Kind::Cancel, {}, 3, 0, 0});
    r[8] = 9;
    putU32(r.data() + 44, crc32c(r.data(), 44));  // valid CRC, invalid kind
    std::uint64_t seq = 0;
    Command out{};
    EXPECT_FALSE(decode(r, seq, out));
}
