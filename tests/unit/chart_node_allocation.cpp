// SPDX-License-Identifier: MIT
//
// Zero-allocation proof for the chart node's per-frame path (issue #183). Same technique as
// tests/unit/chart_series_ring_allocation.cpp (#182) and
// tests/benchmarks/stylesheet/benchmark_counters.cpp: the global operator new/delete pair is
// replaced, so every allocation in this binary is counted - a measured window that reports no
// change really did not allocate, with no sampling, RSS or clock involved.
//
// Construction is the one allowed allocation (the rings and the pre-sized geometry vectors). After
// it, this is the per-frame path and every part of it must allocate zero blocks and zero bytes:
//   append -> rebuild (range resolve, segment/marker rebuild, damage accumulation) -> paint into a
//   counting sink -> the region-scoped paint a partial redraw uses.
//
// GCC at -O3 inlines the replaced operator new/delete pair into std::make_unique and then flags the
// free() inside our operator delete; the pair is intentionally malloc/free (same as the ring test).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

#include "tessera/detail/aligned_alloc.hpp"
#include "tessera/ui/chart/chart_node.hpp"
#include "tessera/ui/chart/chart_render.hpp"
#include "tessera/ui/paint/paint_sink.hpp"

#include <cstddef>
#include <cstdint>
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
        std::fprintf(stderr, "chart_node_allocation: %s\n", message);
    }
    return condition;
}

} // namespace

// Replaced global allocation functions: the only allocation mechanism in this binary, so the
// windows are exact. Deallocations are not counted, because the contract under test is "no
// allocation on the per-frame path", not a memory high-water mark.
void* operator new(std::size_t size) {
    ++g_allocation_count;
    g_allocated_bytes += size;
    if (void* const memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    ++g_allocation_count;
    g_allocated_bytes += size;
    return tessera::detail::aligned_allocate(size, alignment);
}

void operator delete(void* pointer, std::align_val_t) noexcept {
    tessera::detail::aligned_release(pointer);
}

void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
    tessera::detail::aligned_release(pointer);
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

using tessera::rendering::rhi::Rect;
using tessera::ui::chart::ChartDamage;
using tessera::ui::chart::ChartNode;
using tessera::ui::chart::ChartSample;
using tessera::ui::chart::ChartSeriesStyle;
using tessera::ui::paint::PaintSink;

constexpr std::size_t kCapacity = 128;
constexpr std::size_t kSeries = 2;
constexpr Rect kPlot{0.0f, 0.0f, 240.0f, 180.0f};

// Counts the boxes a paint emits without storing them, so the sink itself cannot allocate.
class CountingSink final : public PaintSink {
public:
    void fill_rect(const Rect& /*rect*/, const tessera::rendering::rhi::Color& /*color*/) override {
        ++rects;
    }

    // The second pure virtual on PaintSink; the chart never emits a rounded box, but a sink must
    // define it to be concrete.
    void fill_rounded_rect(const Rect& /*rect*/, const tessera::rendering::rhi::Color& /*color*/,
                           float /*radius*/) override {}

    std::size_t rects = 0;
};

// A chart with both series full: the steady state, where an append evicts the oldest sample.
ChartNode make_full_chart() {
    ChartNode chart(kSeries, kCapacity, kPlot);
    for (std::size_t series = 0; series < kSeries; ++series) {
        chart.set_style(series, ChartSeriesStyle{
                                    tessera::rendering::rhi::rgba8(56, 189, 248),
                                    2.0f,
                                    series == 0 ? 0.0f : 3.0f,
                                });
    }
    for (std::size_t index = 0; index < kCapacity; ++index) {
        for (std::size_t series = 0; series < kSeries; ++series) {
            const float value = static_cast<float>((index * (series + 1)) % 40);
            chart.append(series, ChartSample{static_cast<double>(index), value});
        }
    }
    chart.rebuild();
    return chart;
}

// The acceptance test: after construction, the whole per-frame path is allocation-free.
bool allocation_window_ok() {
    const HeapWindow before = heap_window();
    ChartNode chart = make_full_chart();
    const HeapWindow after_construction = heap_window();
    bool ok = expect(after_construction.allocations > before.allocations,
                     "construction allocates its fixed storage") &&
              expect(after_construction.bytes > before.bytes,
                     "construction allocates the ring and geometry buffers");

    // Steady state: 64 frames of append -> rebuild -> paint, both series, no growth.
    const HeapWindow before_window = heap_window();
    CountingSink sink;
    std::size_t damage_rects = 0;
    std::size_t total_draws = 0;
    for (std::size_t frame = 0; frame < 64; ++frame) {
        const double timestamp = static_cast<double>(kCapacity + frame);
        chart.append(0, ChartSample{timestamp, static_cast<float>(frame % 40)});
        chart.append(1, ChartSample{timestamp, static_cast<float>((frame * 3) % 40)});
        const ChartDamage& damage = chart.rebuild();
        damage_rects += damage.size();
        sink.rects = 0;
        tessera::ui::chart::paint_chart(chart, sink, 1.0f);
        total_draws += sink.rects;
    }
    const HeapWindow after_window = heap_window();

    ok = expect(after_window.allocations == before_window.allocations &&
                    after_window.bytes == before_window.bytes,
                "append, rebuild, damage and paint allocate zero blocks and zero bytes") &&
         ok;
    ok = expect(damage_rects > 0 && total_draws > 0,
                "the measured window really did the work (damage and draws are non-zero)") &&
         ok;
    return ok;
}

// A non-full ring: the append path that only marks one segment. This is the cheap per-sample path a
// live chart actually runs for its first capacity samples, so it is measured separately.
bool grow_window_ok() {
    ChartNode chart(kSeries, kCapacity, kPlot);
    for (std::size_t series = 0; series < kSeries; ++series) {
        chart.set_style(series, ChartSeriesStyle{});
    }
    chart.rebuild();

    const HeapWindow before_window = heap_window();
    CountingSink sink;
    for (std::size_t index = 0; index < kCapacity; ++index) {
        for (std::size_t series = 0; series < kSeries; ++series) {
            chart.append(series,
                         ChartSample{static_cast<double>(index), static_cast<float>(index % 17)});
            chart.rebuild();
        }
        tessera::ui::chart::paint_chart(chart, sink, 1.0f);
    }
    const HeapWindow after_window = heap_window();
    return expect(after_window.allocations == before_window.allocations &&
                      after_window.bytes == before_window.bytes,
                  "growing the ring one sample at a time allocates nothing") &&
           expect(sink.rects > 0, "the growing window painted") &&
           expect(chart.retained_segments() > 0, "the growing window retained segments");
}

// Style, plot and range changes are caller events (a resize, a theme change), not per-frame work,
// but they must not allocate either: they only mark retained geometry dirty.
bool invalidation_window_ok() {
    ChartNode chart = make_full_chart();
    const HeapWindow before_window = heap_window();
    chart.set_style(0, ChartSeriesStyle{tessera::rendering::rhi::rgba8(255, 0, 0), 4.0f, 5.0f});
    chart.rebuild();
    chart.set_plot(Rect{0.0f, 0.0f, 200.0f, 100.0f});
    chart.rebuild();
    chart.set_time_range(tessera::ui::chart::ChartTimeRange{0.0, 200.0});
    chart.rebuild();
    chart.set_value_range(tessera::ui::chart::ChartValueRange{-10.0f, 40.0f});
    chart.rebuild();
    chart.set_auto_ranges();
    chart.rebuild();
    const HeapWindow after_window = heap_window();
    return expect(after_window.allocations == before_window.allocations &&
                      after_window.bytes == before_window.bytes,
                  "invalidation and whole-chart rebuilds allocate nothing");
}

// The immediate-mode hook runs inside the paint call, so a hook that only draws must not allocate
// either. The context is a stack object; the hook writes a counter into it.
struct ImmediateProbe {
    std::size_t calls = 0;
};

void immediate_draw(void* context, const tessera::ui::chart::ChartImmediateDraw& draw) {
    auto* probe = static_cast<ImmediateProbe*>(context);
    ++probe->calls;
    if (draw.sink != nullptr && draw.plot != nullptr) {
        draw.sink->fill_rect(tessera::rendering::rhi::Rect{0.0f, 0.0f, 1.0f, 1.0f},
                             tessera::rendering::rhi::rgba8(255, 255, 255));
    }
}

bool immediate_window_ok() {
    ChartNode chart = make_full_chart();
    ImmediateProbe probe;
    chart.set_immediate(&immediate_draw, &probe);

    const HeapWindow before_window = heap_window();
    CountingSink sink;
    for (std::size_t frame = 0; frame < 16; ++frame) {
        tessera::ui::chart::paint_chart(chart, sink, 1.0f, chart.immediate(),
                                        chart.immediate_context());
        chart.append(0, ChartSample{static_cast<double>(kCapacity + frame), 3.0f});
        chart.rebuild();
    }
    const HeapWindow after_window = heap_window();
    return expect(after_window.allocations == before_window.allocations &&
                      after_window.bytes == before_window.bytes,
                  "painting with an immediate hook allocates nothing") &&
           expect(probe.calls == 16, "the immediate hook ran once per paint") &&
           expect(sink.rects > 16, "the immediate hook drew through the sink");
}

} // namespace

namespace chart_node_test {

bool allocation_free_after_construction() {
    const bool steady = allocation_window_ok();
    const bool growing = grow_window_ok();
    const bool invalidating = invalidation_window_ok();
    const bool immediate = immediate_window_ok();
    return steady && growing && invalidating && immediate;
}

} // namespace chart_node_test