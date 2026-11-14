// SPDX-License-Identifier: MIT
//
// Chart series ring buffer (issue #182, docs/specs/chart-series-v1.md): fixed capacity,
// overwrite-oldest, logical oldest -> newest iteration and the zero-allocation guarantee.
//
// The allocation proof lives in chart_series_ring_allocation.cpp: it replaces the global
// operator new/delete (the technique of tests/benchmarks/stylesheet/benchmark_counters.cpp) and
// asserts that appends, reads, iteration and clear allocate zero blocks and bytes after
// construction. This file covers the storage semantics and the noexcept surface.

#include "tessera/ui/chart/chart_series.hpp"

#include <cstddef>
#include <cstdio>
#include <limits>
#include <type_traits>
#include <utility>

namespace chart_test {

// Defined in chart_series_ring_allocation.cpp; true when the zero-allocation window held.
[[nodiscard]] bool allocation_free_after_construction();

} // namespace chart_test

namespace {

using tessera::ui::chart::ChartSample;
using tessera::ui::chart::ChartSeriesRing;

// The contract is part of the type: appends, reads, iteration and clear never throw, and the ring
// is movable but not copyable (copying would have to allocate and would duplicate the storage).
static_assert(noexcept(std::declval<ChartSeriesRing&>().append(ChartSample{})));
static_assert(noexcept(std::declval<const ChartSeriesRing&>()[0]));
static_assert(noexcept(std::declval<const ChartSeriesRing&>().oldest()));
static_assert(noexcept(std::declval<const ChartSeriesRing&>().newest()));
static_assert(noexcept(std::declval<ChartSeriesRing&>().clear()));
static_assert(noexcept(std::declval<const ChartSeriesRing&>().spans()));
static_assert(noexcept(std::declval<const ChartSeriesRing&>().begin()));
static_assert(noexcept(std::declval<const ChartSeriesRing&>().end()));
static_assert(std::is_nothrow_move_constructible_v<ChartSeriesRing>);
static_assert(std::is_nothrow_move_assignable_v<ChartSeriesRing>);
static_assert(!std::is_copy_constructible_v<ChartSeriesRing>);
static_assert(!std::is_copy_assignable_v<ChartSeriesRing>);

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "chart_series_ring: %s\n", message);
    }
    return condition;
}

ChartSample sample(double timestamp, float value) {
    return ChartSample{timestamp, value};
}

bool capacity_must_be_positive() {
    bool threw = false;
    try {
        const ChartSeriesRing ring(0);
        (void)ring;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    return expect(threw, "capacity 0 throws std::invalid_argument");
}

// Overwrite-oldest, logical order across a wrap, the two spans and capacity 1.
bool overwrite_oldest_and_wrap_order() {
    ChartSeriesRing ring(4);
    bool ok = true;
    for (int index = 1; index <= 6; ++index) {
        ok = ring.append(sample(static_cast<double>(index), static_cast<float>(index))) && ok;
    }
    ok = expect(ring.size() == ring.capacity() && ring.full(),
                "six appends into capacity 4 stay full") &&
         ok;
    ok = expect(ring[0] == sample(3.0, 3.0f) && ring[1] == sample(4.0, 4.0f) &&
                    ring[2] == sample(5.0, 5.0f) && ring[3] == sample(6.0, 6.0f),
                "logical index 0 is the oldest surviving sample") &&
         ok;
    const ChartSeriesRing::SpanPair spans = ring.spans();
    ok = expect(spans[0].size == 2 && spans[1].size == 2, "a wrapped ring returns two spans") && ok;
    ok = expect(spans[0].data[0] == ring[0] && spans[0].data[1] == ring[1] &&
                    spans[1].data[0] == ring[2] && spans[1].data[1] == ring[3],
                "span chunks concatenate to the oldest -> newest order") &&
         ok;
    std::size_t position = 0;
    for (const ChartSample& value : ring) {
        ok = expect(value == ring[position], "iterator yields the indexed order") && ok;
        ++position;
    }

    ChartSeriesRing single(1);
    ok = expect(single.append(sample(1.0, 1.0f)), "capacity 1 accepts its first sample") && ok;
    ok = expect(single.full() && single.oldest() == single.newest(),
                "capacity 1 full ring has one sample") &&
         ok;
    ok = expect(single.append(sample(2.0, 2.0f)), "capacity 1 overwrites its only sample") && ok;
    ok = expect(single.size() == 1 && single.newest() == sample(2.0, 2.0f),
                "capacity 1 keeps the newest sample") &&
         ok;
    return expect(!single.append(sample(2.0, 3.0f)), "capacity 1 rejects an equal timestamp") && ok;
}

bool rejects_non_monotonic_and_non_finite_timestamps() {
    ChartSeriesRing ring(4);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    bool ok = ring.append(sample(10.0, 1.0f)) && ring.append(sample(20.0, 2.0f)) &&
              ring.append(sample(30.0, 3.0f));
    const std::size_t size_before_rejections = ring.size();
    ok = expect(!ring.append(sample(30.0, 4.0f)), "an equal timestamp is rejected") && ok;
    ok = expect(!ring.append(sample(25.0, 4.0f)), "an older timestamp is rejected") && ok;
    ok = expect(!ring.append(sample(nan, 4.0f)), "a NaN timestamp is rejected") && ok;
    ok = expect(!ring.append(sample(infinity, 4.0f)), "an infinite timestamp is rejected") && ok;
    ok = expect(ring.size() == size_before_rejections && ring.newest() == sample(30.0, 3.0f),
                "a rejected append changes nothing") &&
         ok;
    return expect(ring.append(sample(31.0, 4.0f)), "a strictly newer timestamp is accepted") && ok;
}

bool clear_starts_a_new_time_range_without_allocating() {
    ChartSeriesRing ring(4);
    double timestamp = 100.0;
    for (int index = 0; index < 6; ++index) {
        (void)ring.append(sample(timestamp, static_cast<float>(index)));
        timestamp += 1.0;
    }
    ring.clear();
    const bool appended = ring.append(sample(0.0, 9.0f));
    return expect(ring.size() == 1 && appended, "clear allows any finite timestamp afterwards") &&
           expect(ring.capacity() == 4, "clear keeps the capacity") &&
           expect(!ring.append(sample(0.0, 10.0f)), "monotonicity restarts after clear");
}

bool move_keeps_samples_and_inerts_the_source() {
    ChartSeriesRing ring(8);
    for (int index = 0; index < 3; ++index) {
        (void)ring.append(sample(static_cast<double>(index), static_cast<float>(index)));
    }
    ChartSeriesRing moved(std::move(ring));
    ChartSeriesRing target(2);
    target = std::move(moved);
    return expect(target.size() == 3 && target[0] == sample(0.0, 0.0f) &&
                      target[2] == sample(2.0, 2.0f),
                  "a moved ring keeps its samples") &&
           expect(ring.empty() && ring.capacity() == 0 && moved.empty() && moved.capacity() == 0,
                  "the moved-from rings are inert") &&
           expect(!ring.append(sample(9.0, 9.0f)), "an inert moved-from ring rejects appends");
}

} // namespace

int main() {
    bool ok = true;
    ok = chart_test::allocation_free_after_construction() && ok;
    ok = capacity_must_be_positive() && ok;
    ok = overwrite_oldest_and_wrap_order() && ok;
    ok = rejects_non_monotonic_and_non_finite_timestamps() && ok;
    ok = clear_starts_a_new_time_range_without_allocating() && ok;
    ok = move_keeps_samples_and_inerts_the_source() && ok;
    if (!ok) {
        return 1;
    }
    std::printf("chart_series_ring: ok\n");
    return 0;
}
