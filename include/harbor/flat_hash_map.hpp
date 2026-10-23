#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace harbor {

// Fixed-capacity open-addressing hash map keyed by uint64_t.
// Linear probing, load factor <= 0.5, backward-shift deletion (no tombstones).
// All memory is allocated in the constructor; no rehashing ever happens.
template <class V>
class FlatHashMap {
public:
    explicit FlatHashMap(std::size_t maxEntries)
        : maxEntries_(maxEntries),
          mask_(std::bit_ceil(std::max<std::size_t>(2, 2 * maxEntries)) - 1),
          keys_(mask_ + 1),
          values_(mask_ + 1),
          used_(mask_ + 1, 0) {}

    bool insert(std::uint64_t key, V value) {
        if (size_ == maxEntries_) {
            return false;
        }
        std::size_t i = home(key);
        while (used_[i]) {
            if (keys_[i] == key) {
                return false;
            }
            i = (i + 1) & mask_;
        }
        used_[i] = 1;
        keys_[i] = key;
        values_[i] = value;
        ++size_;
        return true;
    }

    V* find(std::uint64_t key) {
        std::size_t i = slotOf(key);
        return i == kNotFound ? nullptr : &values_[i];
    }

    const V* find(std::uint64_t key) const {
        std::size_t i = slotOf(key);
        return i == kNotFound ? nullptr : &values_[i];
    }

    bool erase(std::uint64_t key) {
        std::size_t hole = slotOf(key);
        if (hole == kNotFound) {
            return false;
        }
        // Backward shift: pull later entries of the cluster into the hole when
        // doing so does not move them in front of their home slot.
        std::size_t j = (hole + 1) & mask_;
        while (used_[j]) {
            std::size_t distFromHome = (j - home(keys_[j])) & mask_;
            std::size_t distFromHole = (j - hole) & mask_;
            if (distFromHome >= distFromHole) {
                keys_[hole] = keys_[j];
                values_[hole] = values_[j];
                hole = j;
            }
            j = (j + 1) & mask_;
        }
        used_[hole] = 0;
        --size_;
        return true;
    }

    std::size_t size() const { return size_; }
    std::size_t capacity() const { return mask_ + 1; }

private:
    static constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

    // splitmix64 finalizer: spreads sequential ids across the table.
    static std::uint64_t mix(std::uint64_t x) {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return x;
    }

    std::size_t home(std::uint64_t key) const { return mix(key) & mask_; }

    std::size_t slotOf(std::uint64_t key) const {
        std::size_t i = home(key);
        while (used_[i]) {
            if (keys_[i] == key) {
                return i;
            }
            i = (i + 1) & mask_;
        }
        return kNotFound;
    }

    std::size_t maxEntries_;
    std::size_t mask_;
    std::size_t size_ = 0;
    std::vector<std::uint64_t> keys_;
    std::vector<V> values_;
    std::vector<std::uint8_t> used_;  // separate flag: key 0 is a valid key
};

}  // namespace harbor
