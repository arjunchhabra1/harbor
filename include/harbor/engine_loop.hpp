#pragma once

#include <atomic>
#include <cstdint>
#include "harbor/feed.hpp"
#include "harbor/rings.hpp"
#include "harbor/wakeup.hpp"

namespace harbor {

// The engine thread's work: pops commands from the inbound ring, runs them
// through the FeedEngine, and pushes every released (durable) response,
// execution, and feed event to the outbound ring. Never touches a socket.
class EngineLoop {
public:
    using Clock = FeedEngine::Clock;

    // wakeup (optional) is notified whenever output is pushed, so a sleeping
    // network thread wakes immediately.
    EngineLoop(const BookConfig& cfg, const EngineOptions& options, InboundRing& inbound, OutboundRing& outbound,
               Wakeup* wakeup = nullptr);

    // One pass: up to a bounded number of inbound items, then poll and drain.
    // Returns false once a Stop item has been processed.
    bool runOnce(Clock::time_point now);
    // True if the last runOnce found inbound work (the caller may spin rather than sleep).
    bool busy() const { return busy_; }

    // Safe to read from any thread.
    std::uint64_t lastDurableSeq() const { return durableSeq_.load(std::memory_order_acquire); }
    std::uint64_t lastFeedSeq() const { return feedSeq_.load(std::memory_order_acquire); }

private:
    void push(const OutboundItem& item);  // waits while the outbound ring is full; never drops
    void drainAll();
    void serveSnapshot(std::uint64_t requestId);
    void publishSeqs();

    FeedEngine fe_;
    InboundRing& inbound_;
    OutboundRing& outbound_;
    Wakeup* wakeup_;
    bool pushed_ = false;  // output pushed since the last notify
    bool busy_ = false;
    std::atomic<std::uint64_t> durableSeq_{0};
    std::atomic<std::uint64_t> feedSeq_{0};
};

}  // namespace harbor
