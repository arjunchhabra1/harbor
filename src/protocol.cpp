#include "harbor/protocol.hpp"

#include <cstring>
#include "harbor/bytes.hpp"

namespace harbor::wire {

namespace {

bool validSide(std::uint8_t v) { return v <= 1; }
Side toSide(std::uint8_t v) { return v == 0 ? Side::Buy : Side::Sell; }
std::uint8_t fromSide(Side s) { return s == Side::Buy ? 0 : 1; }

// Per-message layout. Each specialization knows its type, payload size, and
// how to write and read the payload.
template <class M>
struct Codec;

template <>
struct Codec<Login> {
    static constexpr MsgType kType = MsgType::Login;
    static constexpr std::size_t kSize = 4;
    static void write(const Login& m, std::uint8_t* p) { putU32(p, m.clientId); }
    static bool read(const std::uint8_t* p, Login& m) {
        m.clientId = getU32(p);
        return true;
    }
};

template <>
struct Codec<NewOrderMsg> {
    static constexpr MsgType kType = MsgType::NewOrder;
    static constexpr std::size_t kSize = 18;
    static void write(const NewOrderMsg& m, std::uint8_t* p) {
        putU32(p, m.clientOrderId);
        p[4] = fromSide(m.side);
        p[5] = m.tif == TimeInForce::GTC ? 0 : 1;
        putU64(p + 6, static_cast<std::uint64_t>(m.price));
        putU32(p + 14, m.qty);
    }
    static bool read(const std::uint8_t* p, NewOrderMsg& m) {
        if (!validSide(p[4]) || p[5] > 1) {
            return false;
        }
        m = NewOrderMsg{getU32(p), toSide(p[4]), p[5] == 0 ? TimeInForce::GTC : TimeInForce::IOC,
                        static_cast<Price>(getU64(p + 6)), getU32(p + 14)};
        return true;
    }
};

template <>
struct Codec<CancelMsg> {
    static constexpr MsgType kType = MsgType::Cancel;
    static constexpr std::size_t kSize = 4;
    static void write(const CancelMsg& m, std::uint8_t* p) { putU32(p, m.clientOrderId); }
    static bool read(const std::uint8_t* p, CancelMsg& m) {
        m.clientOrderId = getU32(p);
        return true;
    }
};

template <>
struct Codec<ReplaceMsg> {
    static constexpr MsgType kType = MsgType::Replace;
    static constexpr std::size_t kSize = 16;
    static void write(const ReplaceMsg& m, std::uint8_t* p) {
        putU32(p, m.clientOrderId);
        putU64(p + 4, static_cast<std::uint64_t>(m.price));
        putU32(p + 12, m.qty);
    }
    static bool read(const std::uint8_t* p, ReplaceMsg& m) {
        m = ReplaceMsg{getU32(p), static_cast<Price>(getU64(p + 4)), getU32(p + 12)};
        return true;
    }
};

template <>
struct Codec<SnapshotRequest> {
    static constexpr MsgType kType = MsgType::SnapshotRequest;
    static constexpr std::size_t kSize = 0;
    static void write(const SnapshotRequest&, std::uint8_t*) {}
    static bool read(const std::uint8_t*, SnapshotRequest&) { return true; }
};

template <>
struct Codec<LoginAccepted> {
    static constexpr MsgType kType = MsgType::LoginAccepted;
    static constexpr std::size_t kSize = 16;
    static void write(const LoginAccepted& m, std::uint8_t* p) {
        putU64(p, m.lastDurableSeq);
        putU64(p + 8, m.feedSeq);
    }
    static bool read(const std::uint8_t* p, LoginAccepted& m) {
        m = LoginAccepted{getU64(p), getU64(p + 8)};
        return true;
    }
};

template <>
struct Codec<LoginRejected> {
    static constexpr MsgType kType = MsgType::LoginRejected;
    static constexpr std::size_t kSize = 1;
    static void write(const LoginRejected& m, std::uint8_t* p) { p[0] = m.reason; }
    static bool read(const std::uint8_t* p, LoginRejected& m) {
        if (p[0] < LoginRejected::Unknown || p[0] > LoginRejected::DuplicateSession) {
            return false;
        }
        m.reason = p[0];
        return true;
    }
};

template <>
struct Codec<ResponseMsg> {
    static constexpr MsgType kType = MsgType::Response;
    static constexpr std::size_t kSize = 18;
    static void write(const ResponseMsg& m, std::uint8_t* p) {
        putU32(p, m.clientOrderId);
        p[4] = m.command;
        p[5] = static_cast<std::uint8_t>(m.result);
        putU64(p + 6, m.seq);
        putU32(p + 14, m.restingQty);
    }
    static bool read(const std::uint8_t* p, ResponseMsg& m) {
        if (p[4] < 2 || p[4] > 4 || p[5] > static_cast<std::uint8_t>(Reject::BookFull)) {
            return false;
        }
        m = ResponseMsg{getU32(p), p[4], static_cast<Reject>(p[5]), getU64(p + 6), getU32(p + 14)};
        return true;
    }
};

template <>
struct Codec<ExecutionMsg> {
    static constexpr MsgType kType = MsgType::Execution;
    static constexpr std::size_t kSize = 33;
    static void write(const ExecutionMsg& m, std::uint8_t* p) {
        putU32(p, m.clientOrderId);
        putU64(p + 4, m.seq);
        putU64(p + 12, static_cast<std::uint64_t>(m.price));
        putU32(p + 20, m.qty);
        p[24] = m.role;
        putU64(p + 25, m.matchId);
    }
    static bool read(const std::uint8_t* p, ExecutionMsg& m) {
        if (p[24] > 1) {
            return false;
        }
        m = ExecutionMsg{getU32(p), getU64(p + 4), static_cast<Price>(getU64(p + 12)), getU32(p + 20), p[24],
                         getU64(p + 25)};
        return true;
    }
};

template <>
struct Codec<SnapshotHeaderMsg> {
    static constexpr MsgType kType = MsgType::SnapshotHeader;
    static constexpr std::size_t kSize = 12;
    static void write(const SnapshotHeaderMsg& m, std::uint8_t* p) {
        putU64(p, m.feedSeq);
        putU32(p + 8, m.orderCount);
    }
    static bool read(const std::uint8_t* p, SnapshotHeaderMsg& m) {
        m = SnapshotHeaderMsg{getU64(p), getU32(p + 8)};
        return true;
    }
};

template <>
struct Codec<SnapshotOrderMsg> {
    static constexpr MsgType kType = MsgType::SnapshotOrder;
    static constexpr std::size_t kSize = 21;
    static void write(const SnapshotOrderMsg& m, std::uint8_t* p) {
        putU64(p, m.id);
        p[8] = fromSide(m.side);
        putU64(p + 9, static_cast<std::uint64_t>(m.price));
        putU32(p + 17, m.qty);
    }
    static bool read(const std::uint8_t* p, SnapshotOrderMsg& m) {
        if (!validSide(p[8])) {
            return false;
        }
        m = SnapshotOrderMsg{getU64(p), toSide(p[8]), static_cast<Price>(getU64(p + 9)), getU32(p + 17)};
        return true;
    }
};

}  // namespace

std::size_t payloadSize(MsgType type) {
    switch (type) {
        case MsgType::Login: return Codec<Login>::kSize;
        case MsgType::NewOrder: return Codec<NewOrderMsg>::kSize;
        case MsgType::Cancel: return Codec<CancelMsg>::kSize;
        case MsgType::Replace: return Codec<ReplaceMsg>::kSize;
        case MsgType::SnapshotRequest: return Codec<SnapshotRequest>::kSize;
        case MsgType::LoginAccepted: return Codec<LoginAccepted>::kSize;
        case MsgType::LoginRejected: return Codec<LoginRejected>::kSize;
        case MsgType::Response: return Codec<ResponseMsg>::kSize;
        case MsgType::Execution: return Codec<ExecutionMsg>::kSize;
        case MsgType::SnapshotHeader: return Codec<SnapshotHeaderMsg>::kSize;
        case MsgType::SnapshotOrder: return Codec<SnapshotOrderMsg>::kSize;
    }
    return 0;
}

bool isKnownType(std::uint8_t type) {
    return (type >= 1 && type <= 5) || (type >= 101 && type <= 106);
}

template <class M>
void encode(const M& msg, std::vector<std::uint8_t>& out) {
    std::size_t at = out.size();
    out.resize(at + 3 + Codec<M>::kSize);
    putU16(out.data() + at, static_cast<std::uint16_t>(1 + Codec<M>::kSize));
    out[at + 2] = static_cast<std::uint8_t>(Codec<M>::kType);
    Codec<M>::write(msg, out.data() + at + 3);
}

template <class M>
bool decode(std::span<const std::uint8_t> payload, M& msg) {
    return payload.size() == Codec<M>::kSize && Codec<M>::read(payload.data(), msg);
}

#define HARBOR_WIRE_INSTANTIATE(M)                                          \
    template void encode<M>(const M&, std::vector<std::uint8_t>&);          \
    template bool decode<M>(std::span<const std::uint8_t>, M&);
HARBOR_WIRE_INSTANTIATE(Login)
HARBOR_WIRE_INSTANTIATE(NewOrderMsg)
HARBOR_WIRE_INSTANTIATE(CancelMsg)
HARBOR_WIRE_INSTANTIATE(ReplaceMsg)
HARBOR_WIRE_INSTANTIATE(SnapshotRequest)
HARBOR_WIRE_INSTANTIATE(LoginAccepted)
HARBOR_WIRE_INSTANTIATE(LoginRejected)
HARBOR_WIRE_INSTANTIATE(ResponseMsg)
HARBOR_WIRE_INSTANTIATE(ExecutionMsg)
HARBOR_WIRE_INSTANTIATE(SnapshotHeaderMsg)
HARBOR_WIRE_INSTANTIATE(SnapshotOrderMsg)
#undef HARBOR_WIRE_INSTANTIATE

void FrameReader::feed(const std::uint8_t* data, std::size_t n) {
    if (pos_ > 0) {  // drop frames already handed out
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
        pos_ = 0;
    }
    buf_.insert(buf_.end(), data, data + n);
}

FrameReader::Status FrameReader::next(Frame& out) {
    if (error_) {
        return Status::Error;
    }
    if (buf_.size() - pos_ < 3) {
        return Status::NeedMore;
    }
    const std::uint8_t* p = buf_.data() + pos_;
    std::uint16_t length = getU16(p);
    std::uint8_t type = p[2];
    if (!isKnownType(type) || length != 1 + payloadSize(static_cast<MsgType>(type))) {
        error_ = true;
        return Status::Error;
    }
    if (buf_.size() - pos_ < std::size_t{2} + length) {
        return Status::NeedMore;
    }
    out = Frame{static_cast<MsgType>(type), std::span<const std::uint8_t>(p + 3, length - 1u)};
    pos_ += 2 + length;
    return Status::Frame;
}

std::size_t eventSize(FeedEvent::Type type) {
    switch (type) {
        case FeedEvent::Type::Added: return 22;
        case FeedEvent::Type::Executed: return 29;
        case FeedEvent::Type::Deleted: return 9;
        case FeedEvent::Type::Reduced: return 13;
    }
    return 0;
}

bool FeedPacketizer::add(const FeedEvent& e) {
    if (count_ > 0 && e.seq != firstSeq_ + count_) {
        return false;
    }
    if (count_ == 0) {
        buf_.assign(kPacketHeaderSize, 0);
        firstSeq_ = e.seq;
    }
    std::size_t size = eventSize(e.type);
    if (buf_.size() + size > kMaxPacket || count_ == UINT16_MAX) {
        return false;
    }
    std::size_t at = buf_.size();
    buf_.resize(at + size, 0);
    std::uint8_t* p = buf_.data() + at;
    p[0] = static_cast<std::uint8_t>(e.type);
    putU64(p + 1, e.id);
    switch (e.type) {
        case FeedEvent::Type::Added:
            p[9] = fromSide(e.side);
            putU64(p + 10, static_cast<std::uint64_t>(e.price));
            putU32(p + 18, e.qty);
            break;
        case FeedEvent::Type::Executed:
            putU64(p + 9, static_cast<std::uint64_t>(e.price));
            putU32(p + 17, e.qty);
            putU64(p + 21, e.matchId);
            break;
        case FeedEvent::Type::Deleted:
            break;
        case FeedEvent::Type::Reduced:
            putU32(p + 9, e.qty);
            break;
    }
    ++count_;
    return true;
}

std::span<const std::uint8_t> FeedPacketizer::packet() {
    if (count_ == 0) {
        return {};
    }
    putU32(buf_.data(), session_);
    putU64(buf_.data() + 4, firstSeq_);
    putU16(buf_.data() + 12, count_);
    return buf_;
}

void FeedPacketizer::clear() {
    count_ = 0;
    buf_.clear();
}

bool decodePacket(std::span<const std::uint8_t> packet, std::uint32_t& session, std::vector<FeedEvent>& events) {
    events.clear();
    if (packet.size() < kPacketHeaderSize) {
        return false;
    }
    const std::uint8_t* p = packet.data();
    session = getU32(p);
    std::uint64_t first = getU64(p + 4);
    std::uint16_t count = getU16(p + 12);
    std::size_t at = kPacketHeaderSize;
    for (std::uint16_t i = 0; i < count; ++i) {
        if (at >= packet.size()) {
            return false;
        }
        auto type = static_cast<FeedEvent::Type>(p[at]);
        std::size_t size = eventSize(type);
        if (size == 0 || at + size > packet.size()) {
            return false;
        }
        const std::uint8_t* e = p + at;
        FeedEvent ev{type, first + i, getU64(e + 1), Side::Buy, 0, 0, 0};
        switch (type) {
            case FeedEvent::Type::Added:
                if (!validSide(e[9])) {
                    return false;
                }
                ev.side = toSide(e[9]);
                ev.price = static_cast<Price>(getU64(e + 10));
                ev.qty = getU32(e + 18);
                break;
            case FeedEvent::Type::Executed:
                ev.price = static_cast<Price>(getU64(e + 9));
                ev.qty = getU32(e + 17);
                ev.matchId = getU64(e + 21);
                break;
            case FeedEvent::Type::Deleted:
                break;
            case FeedEvent::Type::Reduced:
                ev.qty = getU32(e + 9);
                break;
        }
        events.push_back(ev);
        at += size;
    }
    return at == packet.size();
}

}  // namespace harbor::wire
