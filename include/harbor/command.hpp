#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include "harbor/types.hpp"

namespace harbor {

// One input to a book: exactly what the write-ahead log records.
struct Command {
    enum class Kind : std::uint8_t { Add = 1, Cancel = 2, Replace = 3 } kind;
    NewOrder order;  // Add
    OrderId id;      // Cancel / Replace
    Price price;     // Replace
    Quantity qty;    // Replace
};

// Log record layout (little-endian):
//   0 seq u64 | 8 kind u8 | 9 side u8 | 10 tif u8 | 11 reserved u8 | 12 client u32 |
//   16 order id u64 | 24 price i64 | 32 qty u32 | 36 reserved u64 | 44 CRC32C of bytes 0-43
inline constexpr std::size_t kRecordSize = 48;

void encodeRecord(std::uint64_t seq, const Command& cmd, std::uint8_t* out);

// Returns false if the CRC does not match or a field holds an invalid value.
bool decodeRecord(const std::uint8_t* in, std::uint64_t& seq, Command& cmd);

template <class Book>
Reject applyCommand(Book& book, const Command& cmd, std::vector<Fill>& out) {
    switch (cmd.kind) {
        case Command::Kind::Add:
            return book.add(cmd.order, out);
        case Command::Kind::Cancel:
            out.clear();
            return book.cancel(cmd.id);
        case Command::Kind::Replace:
            return book.replace(cmd.id, cmd.price, cmd.qty, out);
    }
    out.clear();
    return Reject::None;
}

}  // namespace harbor
