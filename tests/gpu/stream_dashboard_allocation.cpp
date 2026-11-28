// SPDX-License-Identifier: MIT
//
// Zero-allocation proof for the streaming dashboard's per-frame path under sustained load (issue
// #184, building on #182/#183). Same technique as tests/unit/chart_node_allocation.cpp: the global
// operator new/delete pair is replaced, so a window that reports no change really did not allocate.
//
// Construction allocates (rings, geometry, accumulator). After a warm-up that wraps every ring, 600
// further frames of the scripted load must allocate nothing in step() (append, rebuild, damage
// accumulation) nor in paint(), whole-scene or through the damage-clipped sink. make_plan() is the
// shared runtime's value type and is deliberately outside the windows.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

#include "stream_dashboard.hpp"
#include "tessera/detail/aligned_alloc.hpp"
#include "tessera/ui/paint/clipped_paint_sink.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {

std::size_t g_allocation_count = 0;
std::size_t g_allocated_bytes = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "stream_dashboard_allocation: %s\n", message);
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

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {

namespace stream = tessera::samples::stream;
namespace rhi = tessera::rendering::rhi;

// Counts boxes and keeps nothing: the only state is two integers.
struct CountingSink final : public tessera::ui::paint::PaintSink {
    std::size_t boxes = 0;
    void fill_rect(const rhi::Rect&, const rhi::Color&) override {
        ++boxes;
    }
    void fill_rounded_rect(const rhi::Rect&, const rhi::Color&, float) override {
        ++boxes;
    }
};

} // namespace

int main() {
    stream::StreamDashboard dashboard;
    CountingSink counting;
    for (int n = 0; n < 200; ++n) {
        dashboard.step();
        dashboard.paint(counting);
    }
    bool ok = expect(dashboard.chart(0).series(0).full(), "the warm-up wrapped the busiest ring");

    std::size_t step_allocations = 0;
    std::size_t paint_allocations = 0;
    std::size_t clipped_allocations = 0;
    std::size_t max_boxes = 0;
    std::size_t partial = 0;
    for (int n = 0; n < 600; ++n) {
        const std::size_t before_step = g_allocation_count;
        dashboard.step();
        step_allocations += g_allocation_count - before_step;

        const tessera::rendering::RedrawPlan plan = dashboard.make_plan();

        counting.boxes = 0;
        const std::size_t before_paint = g_allocation_count;
        dashboard.paint(counting);
        paint_allocations += g_allocation_count - before_paint;
        max_boxes = counting.boxes > max_boxes ? counting.boxes : max_boxes;

        const std::size_t before_clipped = g_allocation_count;
        {
            tessera::ui::paint::ClippedPaintSink clipped(counting, plan.scissors);
            dashboard.paint(clipped);
        }
        clipped_allocations += g_allocation_count - before_clipped;
        partial += plan.full_redraw ? 0 : 1;
    }

    ok = expect(step_allocations == 0, "step() allocated on the sustained path") && ok;
    ok = expect(paint_allocations == 0, "paint() allocated on the sustained path") && ok;
    ok = expect(clipped_allocations == 0, "the damage-clipped paint allocated") && ok;
    ok = expect(max_boxes <= stream::StreamDashboard::max_boxes(),
                "boxes per paint stay bounded by the ring capacity") &&
         ok;
    ok = expect(partial > 0, "the window covered partial redraws") && ok;
    std::printf("stream_dashboard_allocation: step=%zu paint=%zu clipped=%zu max_boxes=%zu\n",
                step_allocations, paint_allocations, clipped_allocations, max_boxes);
    return ok ? 0 : 1;
}
