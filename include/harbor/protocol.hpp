#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include "harbor/types.hpp"

// Harbor's wire protocol. TCP frames are `u16 length | u8 type | payload`,
// where length counts the type byte plus the payload and every type has one
// fixed payload size. UDP feed packets are `session u32 | first seq u64 |
// count u16` followed by `count` events. All integers are little-endian.
namespace harbor::wire {

enum class MsgType : std::uint8_t {
    Login = 1,
    NewOrder = 2,
    Cancel = 3,
    Replace = 4,
    SnapshotRequest = 5,
    LoginAccepted = 101,
    LoginRejected = 102,
    Response = 103,
    Execution = 104,
    SnapshotHeader = 105,
    SnapshotOrder = 106,
};

// Client -> gateway.
struct Login {
    std::uint32_t clientId;
    bool operator==(const Login&) const = default;
};
struct NewOrderMsg {
    std::uint32_t clientOrderId;
    Side side;
    TimeInForce tif;
    Price price;
    Quantity qty;
    bool operator==(const NewOrderMsg&) const = default;
};
struct CancelMsg {
    std::uint32_t clientOrderId;
    bool operator==(const CancelMsg&) const = default;
};
struct ReplaceMsg {
    std::uint32_t clientOrderId;
    Price price;
    Quantity qty;
    bool operator==(const ReplaceMsg&) const = default;
};
struct SnapshotRequest {
    bool operator==(const SnapshotRequest&) const = default;
};

// Gateway -> client.
struct LoginAccepted {
    std::uint64_t lastDurableSeq;
    std::uint64_t feedSeq;
    bool operator==(const LoginAccepted&) const = default;
};
struct LoginRejected {
    enum : std::uint8_t { Unknown = 1, DuplicateSession = 2 };
    std::uint8_t reason;
    bool operator==(const LoginRejected&) const = default;
};
struct ResponseMsg {
    std::uint32_t clientOrderId;
    std::uint8_t command;  // MsgType of the command: 2 new order, 3 cancel, 4 replace
    Reject result;         // None means accepted and durable
    std::uint64_t seq;
    Quantity restingQty;   // left in the book for this order after the command
    bool operator==(const ResponseMsg&) const = default;
};
struct ExecutionMsg {
    enum : std::uint8_t { Aggressor = 0, Resting = 1 };
    std::uint32_t clientOrderId;
    std::uint64_t seq;
    Price price;
    Quantity qty;
    std::uint8_t role;
    std::uint64_t matchId;
    bool operator==(const ExecutionMsg&) const = default;
};
struct SnapshotHeaderMsg {
    std::uint64_t feedSeq;
    std::uint32_t orderCount;
    bool operator==(const SnapshotHeaderMsg&) const = default;
};
struct SnapshotOrderMsg {
    OrderId id;
    Side side;
    Price price;
    Quantity qty;
    bool operator==(const SnapshotOrderMsg&) const = default;
};

std::size_t payloadSize(MsgType type);  // 0 for unknown types (and SnapshotRequest)
bool isKnownType(std::uint8_t type);

// Appends one complete frame to `out`.
template <class M>
void encode(const M& msg, std::vector<std::uint8_t>& out);

// Decodes a frame payload; false if its size or any enum value is invalid.
template <class M>
bool decode(std::span<const std::uint8_t> payload, M& msg);

struct Frame {
    MsgType type;
    std::span<const std::uint8_t> payload;
};

// Reassembles frames from a byte stream that may arrive split or coalesced.
class FrameReader {
public:
    enum class Status { Frame, NeedMore, Error };

    void feed(const std::uint8_t* data, std::size_t n);
    // The returned frame's payload is valid until the next feed() or next().
    // An error (unknown type or wrong length) is permanent.
    Status next(Frame& out);

private:
    std::vector<std::uint8_t> buf_;
    std::size_t pos_ = 0;
    bool error_ = false;
};

// Market-data feed.
struct FeedEvent {
    enum class Type : std::uint8_t { Added = 1, Executed = 2, Deleted = 3, Reduced = 4 };
    Type type;
    std::uint64_t seq;
    OrderId id;
    Side side;              // Added
    Price price;            // Added, Executed
    Quantity qty;           // Added, Executed, Reduced (new qty)
    std::uint64_t matchId;  // Executed
    bool operator==(const FeedEvent&) const = default;
};

inline constexpr std::size_t kMaxPacket = 1400;
inline constexpr std::size_t kPacketHeaderSize = 14;
std::size_t eventSize(FeedEvent::Type type);  // including the type byte

// Packs events with consecutive seqs into one packet of at most kMaxPacket bytes.
class FeedPacketizer {
public:
    explicit FeedPacketizer(std::uint32_t session) : session_(session) {}
    // False if the event does not fit or is not the next seq: flush, then retry.
    bool add(const FeedEvent& event);
    bool empty() const { return count_ == 0; }
    std::span<const std::uint8_t> packet();  // valid until the next add() or clear()
    void clear();
    void setSession(std::uint32_t session) { session_ = session; }

private:
    std::uint32_t session_;
    std::uint64_t firstSeq_ = 0;
    std::uint16_t count_ = 0;
    std::vector<std::uint8_t> buf_;
};

bool decodePacket(std::span<const std::uint8_t> packet, std::uint32_t& session, std::vector<FeedEvent>& events);

}  // namespace harbor::wire
