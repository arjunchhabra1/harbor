#pragma once

#include <cstdint>
#include "harbor/command.hpp"
#include "harbor/protocol.hpp"
#include "harbor/spsc_ring.hpp"
#include "harbor/types.hpp"

namespace harbor {

// Network thread -> engine thread.
struct InboundItem {
    enum class Kind : std::uint8_t { Command, Snapshot, Stop };
    Kind kind;
    Command cmd;              // Command
    std::uint64_t requestId;  // Snapshot: identifies the requesting connection
};

// Engine thread -> network thread. Everything here came from Engine::drain(),
// so it is durable.
struct OutboundItem {
    enum class Kind : std::uint8_t { Response, Execution, Feed, SnapshotHeader, SnapshotOrder, SnapshotEnd };
    Kind kind;
    ClientId client;          // Response, Execution: whose session receives it
    std::uint64_t requestId;  // Snapshot*: which connection asked
    wire::ResponseMsg response;
    wire::ExecutionMsg execution;
    wire::FeedEvent event;
    wire::SnapshotHeaderMsg snapHeader;
    wire::SnapshotOrderMsg snapOrder;
};

inline constexpr std::size_t kRingCapacity = std::size_t{1} << 16;
using InboundRing = SpscRing<InboundItem, kRingCapacity>;
using OutboundRing = SpscRing<OutboundItem, kRingCapacity>;

}  // namespace harbor
