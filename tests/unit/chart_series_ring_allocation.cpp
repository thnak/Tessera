// SPDX-License-Identifier: MIT
//
// Zero-allocation proof for the chart series ring buffer (issue #182,
// docs/specs/chart-series-v1.md). It replaces the global operator new/delete exactly like
// tests/benchmarks/stylesheet/benchmark_counters.cpp (the stylesheet startup benchmark), so every
// allocation in this binary is counted: a measured window that reports no change really did not
// allocate, with no sampling, RSS or clock involved.
//
// Construction is the one allowed allocation (the fixed storage). After it, appends past capacity,
// indexed reads, forward iteration, spans(), oldest()/newest(), clear() and a follow-up append must
// allocate zero blocks and zero bytes. The semantics tests live in chart_series_ring_tests.cpp.
//
// GCC at -O3 inlines the replaced operator new/delete pair into std::make_unique and then flags the
// free() inside our operator delete; the pair is intentionally malloc/free (same as the benchmark).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

#include "tessera/ui/chart/chart_series.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {

std::size_t g_allocation_count = 0;
std::size_t g_allocated_bytes = 0;

struct HeapWindow {
    std::size_t allocations = 0;
    std::size_t bytes = 0;
};

[[nodiscard]] HeapWindow heap_window() noexcept {
    return HeapWindow{g_allocation_count, g_allocated_bytes};
}

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "chart_series_ring: %s\n", message);
    }
    return condition;
}

} // namespace

// Replaced global allocation functions: the only allocation mechanism in this binary, so the
// windows are exact. Deallocations are not counted because the contract under test is "no
// allocation after construction", not a memory high-water mark.
void* operator new(std::size_t size) {
    ++g_allocation_count;
    g_allocated_bytes += size;
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

namespace {

using tessera::ui::chart::ChartSample;
using tessera::ui::chart::ChartSeriesRing;

// The acceptance test: construction allocates the fixed storage once; everything after it is free.
bool allocation_window_ok() {
    const HeapWindow before = heap_window();
    ChartSeriesRing ring(256);
    const HeapWindow after_construction = heap_window();
    bool ok = expect(after_construction.allocations == before.allocations + 1,
                     "construction performs exactly one heap allocation") &&
              expect(after_construction.bytes >= before.bytes + 256 * sizeof(ChartSample),
                     "construction allocates the capacity * sizeof(ChartSample) storage");

    bool accepted = true;
    const HeapWindow before_window = heap_window();
    for (int pass = 0; pass < 5; ++pass) {
        for (std::size_t index = 0; index < 256; ++index) {
            const double timestamp = static_cast<double>(pass) * 256.0 + static_cast<double>(index);
            accepted = ring.append(ChartSample{timestamp, static_cast<float>(index)}) && accepted;
        }
    }

    double indexed_sum = 0.0;
    for (std::size_t index = 0; index < ring.size(); ++index) {
        indexed_sum += static_cast<double>(ring[index].value);
    }
    double iterated_sum = 0.0;
    std::size_t iterated = 0;
    for (const ChartSample& value : ring) {
        iterated_sum += static_cast<double>(value.value);
        ++iterated;
    }
    const ChartSeriesRing::SpanPair spans = ring.spans();
    std::size_t span_samples = 0;
    bool spans_match = true;
    std::size_t position = 0;
    for (const ChartSeriesRing::Span& span : spans) {
        for (std::size_t index = 0; index < span.size; ++index) {
            spans_match = spans_match && span.data[index] == ring[position];
            ++position;
        }
        span_samples += span.size;
    }
    const ChartSample oldest = ring.oldest();
    const ChartSample newest = ring.newest();
    ring.clear();
    const bool reappended = ring.append(ChartSample{-5.0, 7.0f});
    const HeapWindow after_window = heap_window();

    ok = expect(after_window.allocations == before_window.allocations &&
                    after_window.bytes == before_window.bytes,
                "appends, reads, iteration and clear allocate zero blocks and zero bytes") &&
         ok;
    ok = expect(accepted && iterated == 256 && span_samples == 256,
                "five wrap-arounds stayed full and were fully traversed") &&
         ok;
    ok = expect(indexed_sum == iterated_sum && spans_match,
                "indexed, iterated and span order agree") &&
         ok;
    ok = expect(oldest.timestamp == 1024.0 && newest.timestamp == 1279.0,
                "overwrite-oldest leaves the most recent capacity samples") &&
         ok;
    return expect(reappended && ring.size() == 1 && ring.capacity() == 256,
                  "clear keeps capacity and the next append works") &&
           ok;
}

} // namespace

namespace chart_test {

bool allocation_free_after_construction() {
    return allocation_window_ok();
}

} // namespace chart_test
