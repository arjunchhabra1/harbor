#include "harbor/feed_listener.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <iterator>
#include <poll.h>
#include <sys/socket.h>
#include "harbor/bytes.hpp"
#include "harbor/net.hpp"

namespace harbor {

FeedListener::FeedListener(const BookConfig& cfg, FetchSnapshot fetch) : book_(cfg), fetch_(std::move(fetch)) {}

void FeedListener::onPacket(std::span<const std::uint8_t> packet) {
    std::uint32_t session = 0;
    if (!wire::decodePacket(packet, session, scratch_)) {
        ++corrupt_;
        return;
    }
    if (haveSession_ && session != session_) {
        // The server restarted and its seqs started over: nothing we hold is valid.
        ++gaps_;
        synced_ = false;
        pending_.clear();
    }
    haveSession_ = true;
    session_ = session;
    if (!synced_) {
        pending_.insert(pending_.end(), scratch_.begin(), scratch_.end());
        resync();
        return;
    }
    live(scratch_, true);
}

void FeedListener::live(const std::vector<wire::FeedEvent>& events, bool allowResync) {
    for (std::size_t i = 0; i < events.size(); ++i) {
        const wire::FeedEvent& e = events[i];
        if (e.seq <= book_.lastSeq()) {
            continue;  // duplicate, or already covered by the snapshot
        }
        if (e.seq == book_.lastSeq() + 1 && book_.apply(e)) {
            continue;
        }
        // A missing seq (or an event the book cannot apply): start over from a snapshot.
        ++gaps_;
        synced_ = false;
        pending_.assign(events.begin() + static_cast<std::ptrdiff_t>(i), events.end());
        if (allowResync) {
            resync();
        }
        return;
    }
}

void FeedListener::resync() {
    std::uint64_t feedSeq = 0;
    if (!fetch_(feedSeq, snapshotOrders_)) {
        return;  // try again on the next packet
    }
    ++snapshots_;
    if (!book_.loadSnapshot(feedSeq, snapshotOrders_)) {
        return;
    }
    synced_ = true;
    std::vector<wire::FeedEvent> buffered;
    buffered.swap(pending_);
    std::stable_sort(buffered.begin(), buffered.end(),
                     [](const wire::FeedEvent& a, const wire::FeedEvent& b) { return a.seq < b.seq; });
    live(buffered, false);  // at most one snapshot per packet; a new gap waits for the next one
}

bool fetchSnapshotTcp(const std::string& host, std::uint16_t port, std::uint64_t& feedSeq,
                      std::vector<wire::SnapshotOrderMsg>& orders, int timeoutMs) {
    orders.clear();
    try {
        Fd fd = connectTcp(host, port);
        std::vector<std::uint8_t> request;
        wire::encode(wire::SnapshotRequest{}, request);
        if (::send(fd.get(), request.data(), request.size(), sendFlags()) != static_cast<ssize_t>(request.size())) {
            return false;
        }
        wire::FrameReader reader;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        bool haveHeader = false;
        std::uint32_t expected = 0;
        std::vector<std::uint8_t> buf(65'536);
        for (;;) {
            wire::Frame f{};
            auto st = reader.next(f);
            if (st == wire::FrameReader::Status::Error) {
                return false;
            }
            if (st == wire::FrameReader::Status::Frame) {
                if (!haveHeader) {
                    wire::SnapshotHeaderMsg h{};
                    if (f.type != wire::MsgType::SnapshotHeader || !wire::decode(f.payload, h)) {
                        return false;
                    }
                    haveHeader = true;
                    feedSeq = h.feedSeq;
                    expected = h.orderCount;
                    orders.reserve(expected);
                } else {
                    wire::SnapshotOrderMsg o{};
                    if (f.type != wire::MsgType::SnapshotOrder || !wire::decode(f.payload, o)) {
                        return false;
                    }
                    orders.push_back(o);
                }
                if (haveHeader && orders.size() == expected) {
                    return true;
                }
                continue;
            }
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) {
                return false;
            }
            pollfd p{fd.get(), POLLIN, 0};
            if (::poll(&p, 1, static_cast<int>(left.count())) <= 0) {
                return false;
            }
            ssize_t n = ::recv(fd.get(), buf.data(), buf.size(), 0);
            if (n <= 0) {
                return false;
            }
            reader.feed(buf.data(), static_cast<std::size_t>(n));
        }
    } catch (const std::exception&) {
        return false;
    }
}

std::uint32_t nextFeedSession(const std::filesystem::path& dir, SyncMode mode) {
    std::filesystem::create_directories(dir);
    auto path = dir / "feed-session";
    std::uint32_t previous = 0;
    {
        std::ifstream in(path, std::ios::binary);
        std::uint8_t b[4] = {};
        if (in.read(reinterpret_cast<char*>(b), 4)) {
            previous = getU32(b);
        }
    }
    std::uint32_t session = previous + 1;
    auto tmp = dir / "feed-session.tmp";
    std::filesystem::remove(tmp);
    {
        File f = File::create(tmp);
        std::uint8_t b[4];
        putU32(b, session);
        f.writeAll(b, 4);
        f.sync(mode);
    }
    std::filesystem::rename(tmp, path);
    syncDir(dir, mode);
    return session;
}

}  // namespace harbor
