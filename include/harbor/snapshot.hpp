#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include "harbor/file_io.hpp"
#include "harbor/types.hpp"

namespace harbor {

struct RestingOrder {
    OrderId id;
    ClientId client;
    Side side;
    Price price;
    Quantity remaining;

    bool operator==(const RestingOrder&) const = default;
};

// A book's resting orders in price-time order, plus where it sits in the log.
struct Snapshot {
    std::uint64_t segment;  // the log segment that continues after this snapshot
    std::uint64_t lastSeq;  // last command reflected in the orders
    BookConfig cfg;
    std::vector<RestingOrder> orders;
};

template <class Book>
Snapshot captureSnapshot(const Book& book, const BookConfig& cfg, std::uint64_t segment,
                         std::uint64_t lastSeq) {
    Snapshot snap{segment, lastSeq, cfg, {}};
    snap.orders.reserve(book.orderCount());
    book.forEachResting([&](OrderId id, ClientId client, Side side, Price price, Quantity remaining) {
        snap.orders.push_back(RestingOrder{id, client, side, price, remaining});
    });
    return snap;
}

// File layout (little-endian):
//   header (56): magic "HSNP" | version u16 | reserved u16 | segment u64 | last seq u64 |
//                minPrice i64 | maxPrice i64 | tick i64 | maxOrders u32 | order count u32
//   entries (32 each): id u64 | client u32 | side u8 | 3 reserved | price i64 | remaining u32 |
//                reserved u32
//   trailer: CRC32C u32 over every preceding byte
inline constexpr std::size_t kSnapshotHeaderSize = 56;
inline constexpr std::size_t kSnapshotEntrySize = 32;

// Creates `path` (which must not exist), writes the snapshot, and syncs it.
void writeSnapshotFile(const std::filesystem::path& path, const Snapshot& snap, SyncMode mode);

// Throws std::runtime_error on a bad magic, version, size, field, or checksum.
Snapshot readSnapshotFile(const std::filesystem::path& path);

// Re-adds the orders through the book's public add() as GTC orders. Because a
// snapshot is never crossed and each level is re-added in queue order, the
// result is identical. Any reject or fill means the snapshot is corrupt.
template <class Book>
void restoreSnapshot(Book& book, const Snapshot& snap) {
    std::vector<Fill> fills;
    for (const RestingOrder& o : snap.orders) {
        Reject r = book.add(NewOrder{o.id, o.client, o.side, o.price, o.remaining, TimeInForce::GTC}, fills);
        if (r != Reject::None || !fills.empty()) {
            throw std::runtime_error("corrupt snapshot: order " + std::to_string(o.id) +
                                     " could not be restored (" + toString(r) + ")");
        }
    }
}

}  // namespace harbor
