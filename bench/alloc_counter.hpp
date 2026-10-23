#pragma once

#include <cstdint>

namespace harbor::bench {

// Heap allocations made so far by this process. Linking alloc_counter.cpp
// replaces global operator new with a counting version.
std::uint64_t allocationCount();

}  // namespace harbor::bench
