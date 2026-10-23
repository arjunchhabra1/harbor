#pragma once

#include <cstdint>
#include <stdexcept>
#include "harbor/types.hpp"

namespace harbor {

// The set of legal prices for a book: minPrice, minPrice + tick, ... <= maxPrice.
// Validates the config once and maps prices to level indices without signed
// overflow, even at the int64 extremes (offsets are computed in uint64, where
// wraparound is defined, and C++20 makes the conversion back to int64 modular).
class PriceGrid {
public:
    // Leaves room below UINT32_MAX for "no level" sentinels and bitset rounding.
    static constexpr std::uint64_t kMaxLevels = UINT32_MAX - 64;

    explicit PriceGrid(const BookConfig& cfg) : min_(cfg.minPrice), max_(cfg.maxPrice), tick_(cfg.tick) {
        if (cfg.tick <= 0) {
            throw std::invalid_argument("BookConfig: tick must be positive");
        }
        if (cfg.maxPrice < cfg.minPrice) {
            throw std::invalid_argument("BookConfig: maxPrice < minPrice");
        }
        std::uint64_t steps = offset(cfg.maxPrice) / static_cast<std::uint64_t>(cfg.tick);
        if (steps >= kMaxLevels) {
            throw std::invalid_argument("BookConfig: too many price levels");
        }
        levels_ = static_cast<std::uint32_t>(steps + 1);
    }

    std::uint32_t levels() const { return levels_; }

    bool valid(Price price) const {
        return price >= min_ && price <= max_ && offset(price) % static_cast<std::uint64_t>(tick_) == 0;
    }

    // Precondition: valid(price).
    std::uint32_t levelOf(Price price) const {
        return static_cast<std::uint32_t>(offset(price) / static_cast<std::uint64_t>(tick_));
    }

    Price priceOf(std::uint32_t level) const {
        return static_cast<Price>(static_cast<std::uint64_t>(min_) +
                                  static_cast<std::uint64_t>(level) * static_cast<std::uint64_t>(tick_));
    }

private:
    // price - min_ as an exact unsigned distance; requires price >= min_.
    std::uint64_t offset(Price price) const {
        return static_cast<std::uint64_t>(price) - static_cast<std::uint64_t>(min_);
    }

    Price min_;
    Price max_;
    Price tick_;
    std::uint32_t levels_ = 0;
};

}  // namespace harbor
