// SPDX-License-Identifier: MIT
//
// Portable over-aligned raw allocation, for code that replaces the global `operator new` family.
//
// Why this exists: `ui::Node` allocates its objects on a cache-line boundary through the aligned
// form of `operator new` (node.hpp). A program that counts allocations by replacing only
// `operator new(std::size_t)` does not see those, because the default aligned form calls the system
// aligned allocator directly. The allocation-budget tests therefore replace the aligned forms as
// well, and use these two functions so the replacement is one line per form and identical on every
// toolchain (`std::aligned_alloc` is not available on MSVC).

#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace tessera::detail {

// Throws `std::bad_alloc` on failure, like the global operator it backs. `size` is rounded up to a
// multiple of `alignment`, which `std::aligned_alloc` requires.
[[nodiscard]] inline void* aligned_allocate(std::size_t size, std::align_val_t alignment) {
    const auto align = static_cast<std::size_t>(alignment);
#if defined(_MSC_VER)
    void* pointer = _aligned_malloc(size == 0 ? 1 : size, align);
#else
    const std::size_t rounded = ((size == 0 ? 1 : size) + align - 1) / align * align;
    void* pointer = std::aligned_alloc(align, rounded);
#endif
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }
    return pointer;
}

inline void aligned_release(void* pointer) noexcept {
#if defined(_MSC_VER)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

} // namespace tessera::detail
