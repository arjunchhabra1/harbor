#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "harbor/flat_hash_map.hpp"
#include "harbor/types.hpp"

namespace harbor {

// Optimized price-time order book. Every allocation happens in the constructor:
// orders live in a fixed slot pool, price levels in an array indexed by tick
// offset, and an occupancy bitset finds the next best level with ctz/clz.
class FastBook {
public:
    explicit FastBook(const BookConfig& cfg);

    Reject add(const NewOrder& order, std::vector<Fill>& out);
    Reject cancel(OrderId id);
    Reject replace(OrderId id, Price newPrice, Quantity newQty, std::vector<Fill>& out);

    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;
    std::uint64_t depthAt(Side side, Price price) const;
    std::size_t orderCount() const { return count_; }
    bool checkInvariants(std::string& error) const;

private:
    static constexpr std::uint32_t kNone = UINT32_MAX;

    struct Order {
        OrderId id;
        ClientId client;
        Quantity remaining;
        std::uint32_t level;  // index into the side's level array
        std::uint32_t prev;   // pool slot indices; kNone at the ends
        std::uint32_t next;   // also links the free list
        Side side;
    };
    struct Level {
        std::uint32_t head = kNone;
        std::uint32_t tail = kNone;
        std::uint64_t total = 0;
    };

    bool validPrice(Price price) const;
    std::uint32_t levelOf(Price price) const;
    Price priceOf(std::uint32_t level) const;
    bool crosses(Side side, std::uint32_t level) const;

    void execute(OrderId id, ClientId client, Side side, std::uint32_t level, Quantity qty,
                 TimeInForce tif, std::vector<Fill>& out);
    void rest(OrderId id, ClientId client, Side side, std::uint32_t level, Quantity qty);
    // Removes the order in `slot` from its level, the index, and the pool.
    void removeOrder(std::uint32_t slot);
    void onLevelEmptied(Side side, std::uint32_t level);

    std::uint32_t highestBidAtOrBelow(std::uint32_t level) const;
    std::uint32_t lowestAskAtOrAbove(std::uint32_t level) const;

    BookConfig cfg_;
    std::uint32_t numLevels_;

    std::vector<Order> pool_;
    std::uint32_t freeHead_;
    std::size_t count_ = 0;
    FlatHashMap<std::uint32_t> index_;  // order id -> pool slot

    std::vector<Level> bidLevels_;
    std::vector<Level> askLevels_;
    std::vector<std::uint64_t> bidBits_;  // bit set <=> level non-empty
    std::vector<std::uint64_t> askBits_;
    std::uint32_t bestBid_ = kNone;  // cached level indices
    std::uint32_t bestAsk_ = kNone;
};

}  // namespace harbor
