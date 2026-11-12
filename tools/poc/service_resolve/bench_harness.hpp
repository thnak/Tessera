// SPDX-License-Identifier: MIT
//
// Timing, optimizer barriers and allocation counting for the service-resolve microbenchmark.
// Standalone on purpose: it links nothing from Tessera so the numbers measure only the
// resolution strategies (ADR-050 section 3).
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define TESSERA_BENCH_NOINLINE __declspec(noinline)
#else
#define TESSERA_BENCH_NOINLINE [[gnu::noinline]]
#endif

namespace bench {

// Counted by the replaced global operator new in main.cpp.
extern std::size_t g_allocations;

// Forces the compiler to assume all memory changed, so loop-invariant lookups cannot be hoisted
// out of the measured loop and cached pointers are reloaded from their owner every iteration.
inline void clobber_memory() {
#if defined(_MSC_VER) && !defined(__clang__)
    _ReadWriteBarrier();
#else
    asm volatile("" ::: "memory");
#endif
}

inline volatile std::uint64_t g_sink = 0;

inline void keep(std::uint64_t value) {
    g_sink = value;
}

struct Measurement {
    double ns_per_op = 0.0; // median over repetitions
    double min_ns_per_op = 0.0;
    double allocs_per_op = 0.0; // max over repetitions
};

inline constexpr int kRepetitions = 9;

// Runs body(ops) kRepetitions times and reports the median and minimum ns per op. The body
// performs `ops` operations; allocation counts are taken only around the body.
template<typename Body> Measurement measure(std::size_t ops, Body&& body) {
    std::array<double, kRepetitions> samples{};
    double worst_allocs = 0.0;
    body(ops / 10 + 1); // warm caches and the branch predictor
    for (double& sample : samples) {
        std::size_t const allocs_before = g_allocations;
        auto const start = std::chrono::steady_clock::now();
        body(ops);
        auto const stop = std::chrono::steady_clock::now();
        std::size_t const allocs = g_allocations - allocs_before;
        sample = std::chrono::duration<double, std::nano>(stop - start).count() /
                 static_cast<double>(ops);
        worst_allocs =
            std::max(worst_allocs, static_cast<double>(allocs) / static_cast<double>(ops));
    }
    std::ranges::sort(samples);
    return Measurement{samples[kRepetitions / 2], samples[0], worst_allocs};
}

} // namespace bench
