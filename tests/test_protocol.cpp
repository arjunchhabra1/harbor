#include <gtest/gtest.h>
#include <limits>
#include <vector>
#include "harbor/protocol.hpp"
using namespace harbor;
using namespace harbor::wire;

namespace {

constexpr Price kMinPrice = std::numeric_limits<Price>::min();
constexpr Price kMaxPrice = std::numeric_limits<Price>::max();

// Encodes one frame, parses it back with a FrameReader, and decodes it.
template <class M>
M roundTrip(const M& msg, MsgType expectedType) {
    std::vector<std::uint8_t> buf;
    encode(msg, buf);
    FrameReader reader;
    reader.feed(buf.data(), buf.size());
    Frame frame{};
    EXPECT_EQ(reader.next(frame), FrameReader::Status::Frame);
    EXPECT_EQ(frame.type, expectedType);
    M out{};
    EXPECT_TRUE(decode(frame.payload, out));
    EXPECT_EQ(reader.next(frame), FrameReader::Status::NeedMore);
    return out;
}

}  // namespace

TEST(Protocol, EveryMessageRoundTrips) {
    Login login{0xDEADBEEF};
    EXPECT_EQ(roundTrip(login, MsgType::Login), login);
    NewOrderMsg order{0xFFFFFFFF, Side::Sell, TimeInForce::IOC, kMinPrice, 0xFFFFFFFF};
    EXPECT_EQ(roundTrip(order, MsgType::NewOrder), order);
    CancelMsg cancel{17};
    EXPECT_EQ(roundTrip(cancel, MsgType::Cancel), cancel);
    ReplaceMsg replace{18, kMaxPrice, 9};
    EXPECT_EQ(roundTrip(replace, MsgType::Replace), replace);
    SnapshotRequest request{};
    EXPECT_EQ(roundTrip(request, MsgType::SnapshotRequest), request);
    LoginAccepted accepted{~0ULL, 12345};
    EXPECT_EQ(roundTrip(accepted, MsgType::LoginAccepted), accepted);
    LoginRejected rejected{2};
    EXPECT_EQ(roundTrip(rejected, MsgType::LoginRejected), rejected);
    ResponseMsg response{7, 4, Reject::BookFull, ~0ULL, 100};
    EXPECT_EQ(roundTrip(response, MsgType::Response), response);
    ExecutionMsg execution{8, 99, -5, 3, 1, ~0ULL};
    EXPECT_EQ(roundTrip(execution, MsgType::Execution), execution);
    SnapshotHeaderMsg header{1ULL << 40, 65'536};
    EXPECT_EQ(roundTrip(header, MsgType::SnapshotHeader), header);
    SnapshotOrderMsg snapOrder{(7ULL << 32) | 9, Side::Buy, kMinPrice, 1};
    EXPECT_EQ(roundTrip(snapOrder, MsgType::SnapshotOrder), snapOrder);
}

TEST(Protocol, PayloadSizes) {
    EXPECT_EQ(payloadSize(MsgType::Login), 4u);
    EXPECT_EQ(payloadSize(MsgType::NewOrder), 18u);
    EXPECT_EQ(payloadSize(MsgType::Cancel), 4u);
    EXPECT_EQ(payloadSize(MsgType::Replace), 16u);
    EXPECT_EQ(payloadSize(MsgType::SnapshotRequest), 0u);
    EXPECT_EQ(payloadSize(MsgType::LoginAccepted), 16u);
    EXPECT_EQ(payloadSize(MsgType::LoginRejected), 1u);
    EXPECT_EQ(payloadSize(MsgType::Response), 18u);
    EXPECT_EQ(payloadSize(MsgType::Execution), 33u);
    EXPECT_EQ(payloadSize(MsgType::SnapshotHeader), 12u);
    EXPECT_EQ(payloadSize(MsgType::SnapshotOrder), 21u);
    EXPECT_FALSE(isKnownType(77));
    EXPECT_TRUE(isKnownType(5));  // SnapshotRequest has an empty payload but is valid
}

TEST(Protocol, DecodeRejectsBadEnums) {
    std::vector<std::uint8_t> buf;
    encode(NewOrderMsg{1, Side::Buy, TimeInForce::GTC, 100, 1}, buf);
    std::vector<std::uint8_t> badSide(buf.begin() + 3, buf.end());
    badSide[4] = 2;
    NewOrderMsg order{};
    EXPECT_FALSE(decode(std::span<const std::uint8_t>(badSide), order));
    std::vector<std::uint8_t> badTif(buf.begin() + 3, buf.end());
    badTif[5] = 2;
    EXPECT_FALSE(decode(std::span<const std::uint8_t>(badTif), order));

    buf.clear();
    encode(ResponseMsg{1, 2, Reject::None, 1, 0}, buf);
    std::vector<std::uint8_t> badResult(buf.begin() + 3, buf.end());
    badResult[5] = 9;
    ResponseMsg response{};
    EXPECT_FALSE(decode(std::span<const std::uint8_t>(badResult), response));

    std::vector<std::uint8_t> shortPayload(buf.begin() + 3, buf.end() - 1);
    EXPECT_FALSE(decode(std::span<const std::uint8_t>(shortPayload), response));
}

TEST(Protocol, FrameReaderSplitsAndCoalesces) {
    std::vector<std::uint8_t> stream;
    for (std::uint32_t i = 0; i < 50; ++i) {
        switch (i % 4) {
            case 0: encode(NewOrderMsg{i, Side::Buy, TimeInForce::GTC, static_cast<Price>(i), i + 1}, stream); break;
            case 1: encode(CancelMsg{i}, stream); break;
            case 2: encode(ReplaceMsg{i, -static_cast<Price>(i), i}, stream); break;
            case 3: encode(SnapshotRequest{}, stream); break;
        }
    }
    auto parseAll = [](FrameReader& reader, std::vector<std::vector<std::uint8_t>>& frames) {
        Frame f{};
        FrameReader::Status st;
        while ((st = reader.next(f)) == FrameReader::Status::Frame) {
            std::vector<std::uint8_t> one{static_cast<std::uint8_t>(f.type)};
            one.insert(one.end(), f.payload.begin(), f.payload.end());
            frames.push_back(std::move(one));
        }
        EXPECT_EQ(st, FrameReader::Status::NeedMore);
    };
    std::vector<std::vector<std::uint8_t>> whole;
    {
        FrameReader reader;
        reader.feed(stream.data(), stream.size());
        parseAll(reader, whole);
    }
    ASSERT_EQ(whole.size(), 50u);
    for (std::size_t split = 0; split <= stream.size(); ++split) {
        FrameReader reader;
        std::vector<std::vector<std::uint8_t>> got;
        reader.feed(stream.data(), split);
        parseAll(reader, got);
        reader.feed(stream.data() + split, stream.size() - split);
        parseAll(reader, got);
        ASSERT_EQ(got, whole) << "split at " << split;
    }
    FrameReader reader;
    std::vector<std::vector<std::uint8_t>> got;
    for (std::uint8_t byte : stream) {
        reader.feed(&byte, 1);
        parseAll(reader, got);
    }
    EXPECT_EQ(got, whole);
}

TEST(FrameReader, BadLengthIsError) {
    std::vector<std::uint8_t> buf = {7, 0, static_cast<std::uint8_t>(MsgType::Login), 1, 2, 3, 4, 5, 6};
    FrameReader reader;
    reader.feed(buf.data(), buf.size());
    Frame f{};
    EXPECT_EQ(reader.next(f), FrameReader::Status::Error);
}

TEST(FrameReader, UnknownTypeIsError) {
    std::vector<std::uint8_t> buf = {1, 0, 77};
    FrameReader reader;
    reader.feed(buf.data(), buf.size());
    Frame f{};
    EXPECT_EQ(reader.next(f), FrameReader::Status::Error);
}

TEST(Feed, PacketRoundTrip) {
    FeedPacketizer packetizer(42);
    std::vector<FeedEvent> events = {
        {FeedEvent::Type::Added, 100, (1ULL << 32) | 5, Side::Sell, kMinPrice, 7, 0},
        {FeedEvent::Type::Executed, 101, (1ULL << 32) | 5, Side::Buy, -3, 2, 9},
        {FeedEvent::Type::Reduced, 102, (1ULL << 32) | 5, Side::Buy, 0, 4, 0},
        {FeedEvent::Type::Deleted, 103, (1ULL << 32) | 5, Side::Buy, 0, 0, 0},
    };
    for (const auto& e : events) {
        ASSERT_TRUE(packetizer.add(e));
    }
    std::uint32_t session = 0;
    std::vector<FeedEvent> decoded;
    ASSERT_TRUE(decodePacket(packetizer.packet(), session, decoded));
    EXPECT_EQ(session, 42u);
    ASSERT_EQ(decoded.size(), events.size());
    // Fields not carried by an event type decode as zero / Buy.
    EXPECT_EQ(decoded[0], events[0]);
    EXPECT_EQ(decoded[1].id, events[1].id);
    EXPECT_EQ(decoded[1].price, -3);
    EXPECT_EQ(decoded[1].qty, 2u);
    EXPECT_EQ(decoded[1].matchId, 9u);
    EXPECT_EQ(decoded[1].seq, 101u);
    EXPECT_EQ(decoded[2].qty, 4u);
    EXPECT_EQ(decoded[3].type, FeedEvent::Type::Deleted);
    EXPECT_EQ(decoded[3].seq, 103u);
}

TEST(Feed, PacketNeverExceedsMax) {
    FeedPacketizer packetizer(1);
    std::uint64_t seq = 1;
    while (packetizer.add(FeedEvent{FeedEvent::Type::Executed, seq, seq, Side::Buy, 1, 1, seq})) {
        ++seq;
    }
    EXPECT_LE(packetizer.packet().size(), kMaxPacket);
    EXPECT_GT(packetizer.packet().size(), kMaxPacket - eventSize(FeedEvent::Type::Executed));
    packetizer.clear();
    EXPECT_TRUE(packetizer.empty());
    EXPECT_TRUE(packetizer.add(FeedEvent{FeedEvent::Type::Executed, seq, seq, Side::Buy, 1, 1, seq}));
}

TEST(Feed, DecodeRejectsTruncatedPacket) {
    FeedPacketizer packetizer(1);
    packetizer.add(FeedEvent{FeedEvent::Type::Added, 1, 1, Side::Buy, 1, 1, 0});
    auto p = packetizer.packet();
    std::uint32_t session = 0;
    std::vector<FeedEvent> decoded;
    EXPECT_FALSE(decodePacket(p.first(p.size() - 1), session, decoded));
}
