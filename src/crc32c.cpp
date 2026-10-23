#include "harbor/crc32c.hpp"

#include <array>

namespace harbor {

namespace {

constexpr std::array<std::uint32_t, 256> makeTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;  // reflected Castagnoli polynomial
        }
        table[i] = c;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kTable = makeTable();

}  // namespace

void Crc32c::update(const void* data, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint32_t c = state_;
    for (std::size_t i = 0; i < n; ++i) {
        c = kTable[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    state_ = c;
}

std::uint32_t crc32c(const void* data, std::size_t n) {
    Crc32c c;
    c.update(data, n);
    return c.value();
}

}  // namespace harbor
