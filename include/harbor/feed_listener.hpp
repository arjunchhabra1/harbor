#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>
#include "harbor/feed.hpp"
#include "harbor/file_io.hpp"
#include "harbor/protocol.hpp"

namespace harbor {

// Rebuilds the exchange's book from the UDP feed. It starts by fetching a
// snapshot stamped with a feed seq, applies newer events in order, and on any
// gap (a missing seq, a corrupt event, or a new feed session after a server
// restart) throws its book away and fetches a fresh snapshot.
class FeedListener {
public:
    // Fills feedSeq and orders with the exchange's current state; false on failure.
    using FetchSnapshot = std::function<bool(std::uint64_t& feedSeq, std::vector<wire::SnapshotOrderMsg>& orders)>;

    FeedListener(const BookConfig& cfg, FetchSnapshot fetch);

    void onPacket(std::span<const std::uint8_t> packet);

    const FeedBook& book() const { return book_; }
    bool synced() const { return synced_; }
    std::uint64_t gaps() const { return gaps_; }
    std::uint64_t snapshots() const { return snapshots_; }
    std::uint64_t corruptPackets() const { return corrupt_; }

private:
    // Applies events in order; on a gap, buffers the rest and (if allowed) resyncs.
    void live(const std::vector<wire::FeedEvent>& events, bool allowResync);
    void resync();

    FeedBook book_;
    FetchSnapshot fetch_;
    bool synced_ = false;
    bool haveSession_ = false;
    std::uint32_t session_ = 0;
    std::vector<wire::FeedEvent> pending_;  // events received while not synced
    std::vector<wire::FeedEvent> scratch_;
    std::vector<wire::SnapshotOrderMsg> snapshotOrders_;
    std::uint64_t gaps_ = 0;
    std::uint64_t snapshots_ = 0;
    std::uint64_t corrupt_ = 0;
};

// Fetches a snapshot from the exchange's snapshot port.
bool fetchSnapshotTcp(const std::string& host, std::uint16_t port, std::uint64_t& feedSeq,
                      std::vector<wire::SnapshotOrderMsg>& orders, int timeoutMs = 5'000);

// Feed seqs restart after a server restart, so each start gets a new feed
// session number, persisted in <dir>/feed-session. Returns the new number.
std::uint32_t nextFeedSession(const std::filesystem::path& dir, SyncMode mode);

}  // namespace harbor
