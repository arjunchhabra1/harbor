#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "harbor/net.hpp"
#include "harbor/poller.hpp"
#include "harbor/protocol.hpp"
#include "harbor/rings.hpp"

namespace harbor {

struct GatewayOptions {
    std::uint16_t orderPort = 0;     // 0 picks a free port
    std::uint16_t snapshotPort = 0;  // 0 picks a free port
    std::string feedHost = "127.0.0.1";
    std::uint16_t feedPort = 0;      // where feed packets are sent; 0 disables the feed
    std::size_t sendBufferLimit = std::size_t{1} << 20;  // per order session; overflow disconnects it
    std::uint32_t feedSession = 1;
};

// The network thread's work: accepts connections, turns client frames into
// engine commands, and delivers durable responses, executions, feed packets,
// and snapshots. Single-threaded; runOnce is its event loop body.
class Gateway {
public:
    // seqs() returns {lastDurableSeq, lastFeedSeq} for LoginAccepted.
    using Seqs = std::function<std::pair<std::uint64_t, std::uint64_t>()>;

    Gateway(const GatewayOptions& options, InboundRing& inbound, OutboundRing& outbound, Seqs seqs);

    void runOnce(int timeoutMs);

    std::uint16_t orderPort() const { return orderPort_; }
    std::uint16_t snapshotPort() const { return snapshotPort_; }
    std::size_t sessionCount() const { return sessionCount_.load(std::memory_order_relaxed); }

private:
    struct Session {
        Fd fd;
        std::uint64_t token = 0;
        bool snapshot = false;  // a snapshot-port connection
        wire::FrameReader reader;
        ClientId client = 0;    // 0 until login
        std::vector<std::uint8_t> out;
        std::size_t outPos = 0;
        bool wantWrite = false;
        bool closing = false;   // close once `out` is flushed
        bool snapshotAsked = false;
        std::optional<InboundItem> blocked;  // waiting for room in the inbound ring
    };

    void accept(int listenFd, bool snapshot);
    void readFrom(Session& s);
    void processFrames(Session& s);
    void handleFrame(Session& s, const wire::Frame& f);
    bool pushOrBlock(Session& s, const InboundItem& item);  // false: blocked, stop reading
    void routeOutbound();
    void route(const OutboundItem& item);
    void flush(Session& s);
    void updateInterest(Session& s);
    void close(std::uint64_t token);
    template <class M>
    void send(Session& s, const M& msg);
    Session* sessionFor(ClientId client);
    void flushFeed();
    void publishFeed(const wire::FeedEvent& event);

    GatewayOptions opts_;
    InboundRing& inbound_;
    OutboundRing& outbound_;
    Seqs seqs_;
    Poller poller_;
    Fd orderListener_;
    Fd snapshotListener_;
    std::uint16_t orderPort_ = 0;
    std::uint16_t snapshotPort_ = 0;
    Fd feedSocket_;
    sockaddr_in feedTarget_{};
    wire::FeedPacketizer packetizer_;

    std::unordered_map<std::uint64_t, Session> sessions_;
    std::unordered_map<ClientId, std::uint64_t> byClient_;
    std::uint64_t nextToken_ = 16;
    std::vector<PollEvent> events_;
    std::vector<std::uint8_t> readBuf_;
    std::atomic<std::size_t> sessionCount_{0};
};

}  // namespace harbor
