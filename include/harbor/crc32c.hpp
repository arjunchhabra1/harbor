#pragma once

#include <cstddef>
#include <cstdint>

namespace harbor {

// CRC-32C (Castagnoli), the checksum used by iSCSI, ext4, and most storage
// engines. Table-driven software implementation.
class Crc32c {
public:
    void update(const void* data, std::size_t n);
    std::uint32_t value() const { return ~state_; }

private:
    std::uint32_t state_ = 0xFFFFFFFFu;
};

std::uint32_t crc32c(const void* data, std::size_t n);

}  // namespace harbor
