// SPDX-License-Identifier: MIT
//
// Memory measurement probe (issue #13): prints one `name=value` line per budgeted quantity on
// stdout, for scripts/check_memory_budget.py to ratchet against
// tests/golden/perf/memory_budget.json.
//
// Why a separate probe instead of the existing tests: `node_size_tests` asserts the same
// `sizeof(Node)` invariant and `memory_steady_state_tests` measures per-node RSS, but a budget file
// needs *numbers on stdout in one place*, measured the same way on every lane. This probe is that
// one place, and it stays a test target (CTest `memory_sizes`) so a broken probe fails the normal
// suite rather than only the Python gate.
//
// Two kinds of row, deliberately mixed here because they share the same "construct things, measure
// them" shape but not the same reliability:
//
// * **Deterministic.** `sizeof` of the hot types is an ABI constant, and the allocation counters
//   come from a replaced global `operator new` inside one process, so both are exact and identical
//   on every run. These are the rows the gate enforces.
// * **Sampled.** Resident bytes per node come from the OS. They move with page granularity, the
//   allocator, and what else the machine is doing, so the gate only warns on them. They are printed
//   here anyway because a budget file that shows only the easy numbers hides the interesting trend.
//
// Every line is `name=value` with an integer value and no unit suffix; the unit lives in the budget
// file so one number cannot be read in two units. `#` starts a comment, so the header is ignorable.
//
// Usage: memory_sizes_probe [--nodes N] [--idle-cycles N]
// Exit codes: 0 measured, 2 usage error or the platform reports no resident memory.

#include "tessera/detail/aligned_alloc.hpp"
#include "tessera/diagnostics/memory.hpp"
#include "tessera/runtime/frame_counters.hpp"
#include "tessera/runtime/frame_pacer.hpp"
#include "tessera/ui/input.hpp"
#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/overlay/placement.hpp"
#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/background_layers.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

// Set by the CMake target to the active build configuration. The fallback keeps the file compilable
// outside that build (a standalone compile for a quick check) without inventing a configuration.
#if !defined(TESSERA_MEMORY_PROBE_CONFIG)
#define TESSERA_MEMORY_PROBE_CONFIG "unknown"
#endif

namespace {

namespace diag = tessera::diagnostics;
namespace ui = tessera::ui;
namespace style = tessera::ui::style;
namespace paint = tessera::ui::paint;
namespace runtime = tessera::runtime;

// Allocation accounting. Global replacement so every allocation in the process is counted,
// including the ones the standard library makes on behalf of the types under test.
std::size_t g_allocation_count = 0;
std::size_t g_allocation_bytes = 0;

struct Counts {
    std::size_t allocations = 0;
    std::size_t bytes = 0;
};

// Runs `fn` and returns what it allocated. The snapshot is taken outside the measured call so the
// measurement's own bookkeeping is never counted.
template<class Fn> Counts measure(Fn&& fn) {
    const std::size_t allocations = g_allocation_count;
    const std::size_t bytes = g_allocation_bytes;
    fn();
    return Counts{g_allocation_count - allocations, g_allocation_bytes - bytes};
}

void handler(void*, const ui::Event&) {}

// Prints one row. Kept trivial so a row cannot silently change shape.
void row(const char* name, std::size_t value) {
    std::printf("%s=%zu\n", name, value);
}

std::size_t parse_count(int argc, char** argv, const char* flag, std::size_t fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) {
            const long long parsed = std::strtoll(argv[i + 1], nullptr, 10);
            // A negative or zero node count would divide by zero below; fall back instead of
            // producing a NaN row.
            return parsed > 0 ? static_cast<std::size_t>(parsed) : fallback;
        }
    }
    return fallback;
}

// The lazy payloads: each must allocate exactly once, on first use, so a plain node stays cheap and
// toggling a value never reallocates (the behavior node_size_tests asserts).
void lazy_payloads() {
    auto node = std::make_unique<ui::Node>(1);

    const Counts aria = measure([&] { node->set_aria("aria-label", "x"); });
    row("aria_table_bytes", aria.bytes);
    row("aria_table_allocations", aria.allocations);

    const Counts inline_style = measure([&] { node->set_inline_style("width", "10px"); });
    row("inline_style_table_bytes", inline_style.bytes);
    row("inline_style_table_allocations", inline_style.allocations);

    const Counts binding = measure([&] { node->bind_event(ui::EventType::PointerUp, &handler); });
    row("event_binding_table_bytes", binding.bytes);
    row("event_binding_table_allocations", binding.allocations);

    // The paint payload is copied into the node's optional; the probe's own PaintData temporary is
    // constructed before the measurement so only the node's slot is counted.
    ui::PaintData paint;
    paint.text = "hello";
    const Counts paint_slot = measure([&] { node->set_paint_data(paint); });
    row("paint_slot_bytes", paint_slot.bytes);

    // Clearing keeps the tables, so re-setting them must not allocate again.
    node->clear_aria();
    node->clear_inline_style();
    node->clear_event_bindings();
    const Counts reuse = measure([&] {
        node->set_aria("aria-label", "y");
        node->set_inline_style("width", "20px");
        node->bind_event(ui::EventType::PointerDown, &handler);
    });
    row("cleared_table_reallocations", reuse.allocations);
}

void sizes() {
    row("sizeof_node", sizeof(ui::Node));
    row("sizeof_input_event", sizeof(ui::InputEvent));
    row("sizeof_paint_data", sizeof(ui::PaintData));
    row("sizeof_computed_style", sizeof(style::ComputedStyle));
    row("sizeof_node_id", sizeof(ui::NodeId));
    // The layout engine's per-node input and output records, one of each per node per layout pass
    // (object-size rules in docs/guides/code-style.md). Pinned so a member added to either shows up.
    row("sizeof_layout_input", sizeof(ui::layout::LayoutInput));
    row("sizeof_layout_box", sizeof(ui::layout::LayoutBox));
    // The paint cache's recorded op (issue #731). It was 2376 bytes while every payload sat inline;
    // the cache charges `ops.capacity() * sizeof(PaintOp)` against a 2 MiB budget, so this number
    // decides how many nodes the cache can hold.
    row("sizeof_paint_op", sizeof(paint::PaintOp));
    row("sizeof_paint_record", sizeof(paint::PaintRecord));
    // The resolved `background-image` value (issue #731). `resolve_layers` returns one by value per
    // painted node, so this number multiplies by the node count on a gradient-heavy screen.
    row("sizeof_background_layer_list", sizeof(style::BackgroundLayerList));
    // The internal-padding rows of issue #731: each was reordered widest-member-first, which is
    // what removed the padding these numbers used to carry.
    row("sizeof_pace_decision", sizeof(runtime::PaceDecision));
    row("sizeof_adaptive_quality_sample", sizeof(runtime::AdaptiveQualitySample));
    row("sizeof_overlay_placement", sizeof(ui::OverlayPlacement));

    // `ComputedStyle` is a `std::map` handle, so its sizeof is the handle and not the per-node
    // style cost: the cost is the first insert's map node plus its two strings. Both are
    // deterministic, and the second one is the number that actually multiplies by the node count.
    style::ComputedStyle computed;
    const Counts one_property = measure([&] { computed.set("color", "#ff0000"); });
    row("computed_style_one_property_bytes", one_property.bytes);
    row("computed_style_one_property_allocations", one_property.allocations);

    // A plain node allocates itself and nothing else. Counted from a null unique_ptr so the
    // measurement is a single `new`.
    std::unique_ptr<ui::Node> plain;
    const Counts plain_cost = measure([&] { plain = std::make_unique<ui::Node>(1); });
    row("plain_node_allocations", plain_cost.allocations);
    // The size the allocator was asked for; <= sizeof(Node) + a debug-STL container proxy.
    row("plain_node_bytes", plain_cost.bytes);
}

// Per-node resident cost and the idle leak guard: the `memory_steady_state` quantities, printed so
// the warn rows have a measurement on this lane too.
void resident(std::size_t nodes, std::size_t idle_cycles) {
    auto root = std::make_unique<ui::Node>(1);
    const diag::ProcessMemory before = diag::sample_process_memory();
    for (std::size_t i = 0; i < nodes; ++i) {
        root->add_child(std::make_unique<ui::Node>(static_cast<ui::NodeId>(i + 2)));
    }
    const diag::ProcessMemory built = diag::sample_process_memory();

    if (!before.valid || !built.valid) {
        // The gate treats a missing warn row as a warning, but a probe that cannot read memory is a
        // setup failure and must not look like a pass.
        std::fprintf(stderr, "memory_sizes_probe: the platform reports no resident memory\n");
        std::exit(2);
    }
    const std::size_t grown =
        built.resident_bytes > before.resident_bytes
            ? static_cast<std::size_t>(built.resident_bytes - before.resident_bytes)
            : 0;
    row("resident_bytes_per_node", nodes == 0 ? 0 : grown / nodes);
    row("resident_bytes_total", grown);

    const auto cycle = [&] {
        root->mark_dirty(ui::Dirty::Paint);
        root->clear_dirty(ui::Dirty::All);
    };
    cycle();
    const diag::ProcessMemory warm = diag::sample_process_memory();
    for (std::size_t i = 0; i < idle_cycles; ++i) {
        cycle();
    }
    const diag::ProcessMemory steady = diag::sample_process_memory();
    row("node_dirty_scratch_bytes",
        steady.resident_bytes > warm.resident_bytes
            ? static_cast<std::size_t>(steady.resident_bytes - warm.resident_bytes)
            : 0);
}

} // namespace

void* operator new(std::size_t size) {
    ++g_allocation_count;
    g_allocation_bytes += size;
    if (void* pointer = std::malloc(size)) {
        return pointer;
    }
    throw std::bad_alloc();
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    ++g_allocation_count;
    g_allocation_bytes += size;
    return tessera::detail::aligned_allocate(size, alignment);
}

void operator delete(void* pointer, std::align_val_t) noexcept {
    tessera::detail::aligned_release(pointer);
}

void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
    tessera::detail::aligned_release(pointer);
}

void operator delete(void* pointer) noexcept {
    std::free(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

int main(int argc, char** argv) {
    const std::size_t nodes = parse_count(argc, argv, "--nodes", 20000);
    const std::size_t idle_cycles = parse_count(argc, argv, "--idle-cycles", 300);

    std::printf("# memory_sizes_probe: name=value rows for scripts/check_memory_budget.py\n");
    // The configuration is a comment, not a `name=value` row, because the gate's numeric rows must
    // stay numeric. It is still reported: a Debug build has a different sizeof(Node) and a larger
    // PaintData, so a Debug number is a different claim from the committed Release budget, and the
    // gate refuses to compare across the two instead of pretending they are the same.
    std::printf("# config=%s\n", TESSERA_MEMORY_PROBE_CONFIG);
    std::printf("# node=%zu idle_cycles=%zu\n", nodes, idle_cycles);
    sizes();
    lazy_payloads();
    resident(nodes, idle_cycles);
    return 0;
}
