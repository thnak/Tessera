// SPDX-License-Identifier: MIT

#include "benchmark_counters.hpp"

#include <cstdlib>
#include <new>

// GCC at -O3 inlines the replaced operator new/delete pair into std::make_unique and then flags the
// free() inside our operator delete; the pair is intentionally malloc/free.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

namespace tessera::benchmark {

std::size_t g_allocation_count = 0;
std::size_t g_allocated_bytes = 0;

std::size_t allocation_count() noexcept {
    return g_allocation_count;
}

std::size_t allocated_bytes() noexcept {
    return g_allocated_bytes;
}

} // namespace tessera::benchmark

// Replaced global allocation functions: every heap allocation in the process is counted, so the
// benchmark can compare the prepare stage of the two startup paths without a sampling profiler.
void* operator new(std::size_t size) {
    ++tessera::benchmark::g_allocation_count;
    tessera::benchmark::g_allocated_bytes += size;
    if (void* const memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t /*size*/) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t /*size*/) noexcept {
    std::free(memory);
}
