#include "harbor/engine_loop.hpp"

#include <thread>

namespace harbor {

namespace {
constexpr int kMaxItemsPerPass = 4'096;
}  // namespace

EngineLoop::EngineLoop(const BookConfig& cfg, const EngineOptions& options, InboundRing& inbound,
                       OutboundRing& outbound)
    : fe_(cfg, options), inbound_(inbound), outbound_(outbound) {
    publishSeqs();
}

void EngineLoop::push(const OutboundItem& item) {
    while (!outbound_.tryPush(item)) {
        std::this_thread::yield();
    }
}

void EngineLoop::publishSeqs() {
    durableSeq_.store(fe_.lastDurableSeq(), std::memory_order_release);
    feedSeq_.store(fe_.lastFeedSeq(), std::memory_order_release);
}

void EngineLoop::drainAll() {
    fe_.drain(
        [&](const wire::FeedEvent& e) {
            OutboundItem item{};
            item.kind = OutboundItem::Kind::Feed;
            item.event = e;
            push(item);
        },
        [&](const wire::ResponseMsg& r, ClientId client) {
            OutboundItem item{};
            item.kind = OutboundItem::Kind::Response;
            item.client = client;
            item.response = r;
            push(item);
        },
        [&](const wire::ExecutionMsg& x, ClientId client) {
            OutboundItem item{};
            item.kind = OutboundItem::Kind::Execution;
            item.client = client;
            item.execution = x;
            push(item);
        });
    publishSeqs();
}

// A snapshot must describe exactly the state the feed has announced: commit
// so nothing undurable is in the book, emit everything released, then stamp
// the snapshot with the last feed seq emitted.
void EngineLoop::serveSnapshot(std::uint64_t requestId) {
    fe_.commit();
    drainAll();
    OutboundItem item{};
    item.kind = OutboundItem::Kind::SnapshotHeader;
    item.requestId = requestId;
    item.snapHeader = wire::SnapshotHeaderMsg{fe_.lastFeedSeq(), static_cast<std::uint32_t>(fe_.book().orderCount())};
    push(item);
    fe_.book().forEachResting([&](OrderId id, ClientId, Side side, Price price, Quantity qty) {
        OutboundItem order{};
        order.kind = OutboundItem::Kind::SnapshotOrder;
        order.requestId = requestId;
        order.snapOrder = wire::SnapshotOrderMsg{id, side, price, qty};
        push(order);
    });
    OutboundItem end{};
    end.kind = OutboundItem::Kind::SnapshotEnd;
    end.requestId = requestId;
    push(end);
}

bool EngineLoop::runOnce(Clock::time_point now) {
    busy_ = false;
    InboundItem item{};
    for (int i = 0; i < kMaxItemsPerPass && inbound_.tryPop(item); ++i) {
        busy_ = true;
        switch (item.kind) {
            case InboundItem::Kind::Command:
                fe_.submit(item.cmd, now);
                break;
            case InboundItem::Kind::Snapshot:
                serveSnapshot(item.requestId);
                break;
            case InboundItem::Kind::Stop:
                fe_.commit();
                drainAll();
                return false;
        }
    }
    fe_.poll(now);
    drainAll();
    return true;
}

}  // namespace harbor
