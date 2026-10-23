#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "harbor/price_grid.hpp"
#include "harbor/types.hpp"

namespace harbor {

// Simple, obviously-correct order book used as the oracle for FastBook.
// Optimized for readability, not speed: it allocates on every resting order.
class ReferenceBook {
public:
    // Throws std::invalid_argument for an invalid config (see PriceGrid).
    explicit ReferenceBook(const BookConfig& cfg);

    Reject add(const NewOrder& order, std::vector<Fill>& out);
    Reject cancel(OrderId id);
    Reject replace(OrderId id, Price newPrice, Quantity newQty, std::vector<Fill>& out);

    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;
    std::uint64_t depthAt(Side side, Price price) const;
    std::size_t orderCount() const { return index_.size(); }
    bool contains(OrderId id) const { return index_.contains(id); }
    bool checkInvariants(std::string& error) const;

private:
    struct Resting {
        OrderId id;
        ClientId client;
        Quantity remaining;
    };
    using Queue = std::list<Resting>;
    struct Locator {
        Side side;
        Price price;
        Queue::iterator it;
    };

    bool crosses(Side side, Price price) const;
    // Matches an aggressor against the opposite side, then rests any remainder
    // if it is GTC. Used by both add and the re-entry path of replace.
    void execute(OrderId id, ClientId client, Side side, Price price, Quantity qty,
                 TimeInForce tif, std::vector<Fill>& out);
    void rest(OrderId id, ClientId client, Side side, Price price, Quantity qty);
    void remove(OrderId id);

    BookConfig cfg_;
    PriceGrid grid_;
    std::map<Price, Queue, std::greater<>> bids_;  // best = begin()
    std::map<Price, Queue> asks_;                  // best = begin()
    std::unordered_map<OrderId, Locator> index_;
};

}  // namespace harbor
