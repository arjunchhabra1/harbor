#include "alloc_counter.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

namespace {
std::atomic<std::uint64_t> gAllocations{0};
}  // namespace

std::uint64_t harbor::bench::allocationCount() {
    return gAllocations.load(std::memory_order_relaxed);
}

// Count every heap allocation in the process.
void* operator new(std::size_t size) {
    gAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    gAllocations.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(size == 0 ? 1 : size);
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return operator new(size, tag);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
