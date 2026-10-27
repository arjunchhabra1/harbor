#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace harbor {

// Lock-free single-producer/single-consumer ring of fixed capacity. One thread
// may call tryPush and one other thread tryPop. Each index is written by only
// one side: the producer publishes tail_ with release after writing the slot,
// and the consumer reads it with acquire before reading the slot (and the
// reverse for head_). Each side also caches the other's index so the common
// case touches only its own cache line.
template <class T, std::size_t Capacity>
class SpscRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    bool tryPush(const T& item) {
        std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - headCache_ == Capacity) {
            headCache_ = head_.load(std::memory_order_acquire);
            if (tail - headCache_ == Capacity) {
                return false;
            }
        }
        slots_[tail & kMask] = item;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool tryPop(T& out) {
        std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tailCache_) {
            tailCache_ = tail_.load(std::memory_order_acquire);
            if (head == tailCache_) {
                return false;
            }
        }
        out = slots_[head & kMask];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Exact when called by either side while the other is idle.
    std::size_t sizeApprox() const {
        return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;
    static constexpr std::size_t kCacheLine = 64;

    alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // written by the consumer
    std::size_t tailCache_ = 0;                             // consumer's copy of tail_
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // written by the producer
    std::size_t headCache_ = 0;                             // producer's copy of head_
    alignas(kCacheLine) std::array<T, Capacity> slots_{};
};

}  // namespace harbor
