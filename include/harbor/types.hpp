#pragma once

#include <cstdint>

namespace harbor {

using Price    = std::int64_t;   // in ticks
using Quantity = std::uint32_t;
using OrderId  = std::uint64_t;
using ClientId = std::uint32_t;

enum class Side { Buy, Sell };
enum class TimeInForce { GTC, IOC };

struct NewOrder {
    OrderId id;
    ClientId client;
    Side side;
    Price price;
    Quantity qty;
    TimeInForce tif;
};

struct Fill {
    OrderId aggressor;
    OrderId resting;
    Price price;
    Quantity qty;

    bool operator==(const Fill&) const = default;
};

enum class Reject { None, DuplicateId, UnknownOrder, BadPrice, BadQuantity, BookFull };

// Valid prices are minPrice, minPrice + tick, ..., up to maxPrice.
struct BookConfig {
    Price minPrice;
    Price maxPrice;
    Price tick;
    std::uint32_t maxOrders;
};

inline const char* toString(Reject r) {
    switch (r) {
        case Reject::None:         return "None";
        case Reject::DuplicateId:  return "DuplicateId";
        case Reject::UnknownOrder: return "UnknownOrder";
        case Reject::BadPrice:     return "BadPrice";
        case Reject::BadQuantity:  return "BadQuantity";
        case Reject::BookFull:     return "BookFull";
    }
    return "Unknown";
}

}  // namespace harbor
