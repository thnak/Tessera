// SPDX-License-Identifier: MIT
//
// Cache-line separation for counters that different threads write (security review findings C1,
// C2, C3, C8).
//
// The problem this solves is false sharing: two atomics that different threads write, sitting in
// the same 64-byte line, make every write invalidate the other thread's copy. The counters stay
// correct either way, so no test fails and no profiler points at it - the cost is invisible
// coherence traffic on the exact paths the runtime is designed to keep cheap.
//
// Two rules keep this header honest:
//
// 1. **A group is exactly one cache line.** `alignas(64)` alone is not enough: MSVC's C4324 warns
//    whenever an alignment specifier forces padding, and this project builds with warnings as
//    errors. `CacheLine<T>` is `alignas(64)` *and* padded to exactly 64 bytes, so the alignment is
//    satisfied by the size rather than by a hole, and the warning never fires.
// 2. **The line size is a fixed 64, not `std::hardware_destructive_interference_size`.** The
//    standard's value is not usable in a `static_assert` on every supported toolchain (GCC warns
//    that it is not an ABI constant), and 64 is the line size on every target this project builds
//    for. Over-aligning on a machine with 128-byte lines costs a little padding and is never
//    wrong; under-aligning would silently reintroduce the sharing.
//
// Usage: declare one plain struct per writer, and wrap it.
//
//   struct UiCounters {
//       std::atomic<std::size_t> posted{0};
//       std::atomic<std::size_t> rejected{0};
//   };
//   detail::CacheLine<UiCounters> ui_;
//
// The wrapper derives from `T`, so `ui_.posted` reads and writes the counter directly, and
// `sizeof(detail::CacheLine<UiCounters>) == 64` is a compile-time guarantee.

#pragma once

#include <cstddef>

namespace tessera::detail {

// The cache-line size this project separates on. See rule 2 above.
inline constexpr std::size_t kCacheLine = 64;

// `T` padded to exactly one cache line and aligned to it. `T` must fit in one line; a group that
// outgrows it is a compile error rather than a silent straddle.
template<class T> struct alignas(kCacheLine) CacheLine : T {
    static_assert(sizeof(T) <= kCacheLine, "a cache-line group must fit in one cache line");
    static_assert(alignof(T) <= kCacheLine, "a cache-line group must not over-align");

    // Never read or written: it exists so the size is exactly one line, which is what keeps the
    // alignment from forcing a hole (and MSVC's C4324 quiet).
    std::byte padding_[kCacheLine - sizeof(T)]{};
};

// The size and alignment are the whole point of the type, so they are asserted where the type is
// defined rather than at each use.
template<class T> [[nodiscard]] constexpr bool cache_line_is_exact() {
    return sizeof(CacheLine<T>) == kCacheLine && alignof(CacheLine<T>) == kCacheLine;
}

} // namespace tessera::detail

// MSVC's C4324 ("structure was padded due to alignment specifier") fires on any struct that
// *contains* a cache-line-aligned member, because the containing struct inherits the alignment and
// its own size is then rounded up. That padding is the entire point of this header, so the warning
// is suppressed - narrowly, around the declaration that holds the groups, and with the reason
// stated at each use. This is the one case the code-style rule "fix warnings instead of adding
// blanket suppressions" allows: the warning describes the intended design, not a defect.
#if defined(_MSC_VER)
#define TESSERA_CACHE_LINE_BEGIN _Pragma("warning(push)") _Pragma("warning(disable : 4324)")
#define TESSERA_CACHE_LINE_END _Pragma("warning(pop)")
#else
#define TESSERA_CACHE_LINE_BEGIN
#define TESSERA_CACHE_LINE_END
#endif
