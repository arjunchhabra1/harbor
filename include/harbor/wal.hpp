#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include "harbor/command.hpp"
#include "harbor/file_io.hpp"

namespace harbor {

// Segment header layout (little-endian):
//   0 magic "HWAL" | 4 version u16 | 6 reserved u16 | 8 segment u64 | 16 first seq u64 |
//   24 reserved u32 | 28 CRC32C of bytes 0-27
inline constexpr std::size_t kWalHeaderSize = 32;

void writeWalHeader(File& file, std::uint64_t segment, std::uint64_t firstSeq);

struct WalScan {
    std::uint64_t segment;
    std::uint64_t firstSeq;
    std::uint64_t nextSeq;      // seq after the last valid record
    std::uint64_t validBytes;   // header + valid records; truncate here to drop a torn tail
    bool tornTail;              // the file has bytes past validBytes
};

// Calls onRecord for each valid record in order. A torn tail (an incomplete
// last record, or bad records running to the end of the file) is reported, not
// thrown: those commands were never acknowledged. Throws std::runtime_error for
// a bad header, a seq gap, or a bad record followed by any valid one.
WalScan scanWal(const std::filesystem::path& path,
                const std::function<void(std::uint64_t seq, const Command& cmd)>& onRecord);

}  // namespace harbor
