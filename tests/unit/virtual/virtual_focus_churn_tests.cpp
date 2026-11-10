// SPDX-License-Identifier: MIT
//
// Keyed focus/selection churn (`docs/specs/virtualization-v1.md` sections 7, 11 and 12): after the
// pool is warm the focus/selection/bookkeeping path performs no heap allocation, node count stays
// inside `pool_cap`, at most one realized node is focused, and the same script produces identical
// step records twice.

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/pseudo_state.hpp"
#include "tessera/ui/virtual/virtual_range.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <utility>
#include <vector>

namespace {

std::size_t g_allocations = 0;

using tessera::ui::Node;
using tessera::ui::style::has_state;
using tessera::ui::style::PseudoState;
using tessera::ui::virtualization::FocusStep;
using tessera::ui::virtualization::MeasureConstraints;
using tessera::ui::virtualization::VirtualDataSource;
using tessera::ui::virtualization::VirtualItemId;
using tessera::ui::virtualization::VirtualItemMeasure;
using tessera::ui::virtualization::VirtualRange;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "virtual_focus_churn: " << message << "\n";
    }
    return condition;
}

constexpr std::size_t kItemCount = 10000;
constexpr std::size_t kPoolCap = 32;
constexpr std::size_t kSelectionKeys = 40; // < state_capacity, so a refusal never happens here
constexpr float kViewport = 240.0f;
constexpr float kOverscan = 200.0f;
constexpr float kMinMain = 24.0f;

class ChurnSource final : public VirtualDataSource {
public:
    explicit ChurnSource(bool record)
        : record_(record) {}

    [[nodiscard]] std::size_t item_count() const override {
        return kItemCount;
    }
    [[nodiscard]] VirtualItemId item_key(std::size_t index) const override {
        return VirtualItemId{500000 + index};
    }
    [[nodiscard]] VirtualItemMeasure measure(std::size_t index,
                                             const MeasureConstraints& constraints) const override {
        return VirtualItemMeasure{24.0f + static_cast<float>(index % 5) * 6.0f,
                                  constraints.cross_extent};
    }
    void build(std::size_t index, Node& out) override {
        out.set_element_kind("row");
        if (record_) {
            build_order.push_back(index);
        }
    }
    void release(std::size_t index, Node&) noexcept override {
        if (record_) {
            release_order.push_back(index);
        }
    }

    std::vector<std::size_t> build_order;
    std::vector<std::size_t> release_order;

private:
    bool record_ = false;
};

VirtualRange::Config churn_config() {
    VirtualRange::Config config;
    config.capacity = kItemCount;
    config.pool_cap = kPoolCap;
    config.state_capacity = 64;
    config.estimate_main = 24.0f;
    config.gap = 4.0f;
    config.overscan_main = kOverscan;
    config.cross_extent = 200.0f;
    return config;
}

std::size_t focus_bits(const Node& container) {
    std::size_t count = 0;
    for (const auto& child : container.children()) {
        if (has_state(child->pseudo_state(), PseudoState::Focus)) {
            ++count;
        }
    }
    return count;
}

struct StepRecord {
    std::size_t index = 0;
    std::size_t realized_first = 0;
    std::size_t realized_last = 0;
    std::size_t selected = 0;
    std::size_t built = 0;
    std::size_t released = 0;
    float scroll = 0.0f;
    bool realized = false;

    bool operator==(const StepRecord&) const = default;
};

struct ScriptResult {
    std::vector<StepRecord> steps;
    std::vector<std::size_t> build_order;
    std::vector<std::size_t> release_order;
    std::size_t focused_key = 0;
    std::size_t selected_first = 0;
    float total_extent = 0.0f;
};

// One deterministic script of focus/selection/scroll steps. `step_count` bounds the run; the
// offsets and keys are derived from the step number only (no clock, no RNG).
ScriptResult run_script(std::size_t step_count, bool record) {
    ChurnSource source(record);
    Node container;
    VirtualRange range(churn_config(), source, container);
    ScriptResult result;
    if (!range.sync().has_value()) {
        return result;
    }
    (void)range.update(0.0f, kViewport);
    result.steps.reserve(step_count);
    std::array<VirtualItemId, 64> selected{};
    for (std::size_t step = 0; step < step_count; ++step) {
        const std::size_t index = (step * 37) % kItemCount;
        const auto focused = range.focus_item(range.key_at(index), kViewport);
        if (!focused.has_value()) {
            return result;
        }
        StepRecord step_record;
        step_record.index = focused->index;
        step_record.scroll = focused->scroll_offset;
        step_record.realized = focused->realized;
        step_record.built = focused->built;
        step_record.released = focused->released;
        if (step % 4 == 0) {
            const FocusStep step_kind = step % 8 == 0 ? FocusStep::Next : FocusStep::Previous;
            const auto stepped = range.step_focus(step_kind, kViewport);
            if (!stepped.has_value()) {
                return result;
            }
            step_record.index = stepped->index;
            step_record.scroll = stepped->scroll_offset;
            step_record.realized = stepped->realized;
            step_record.built += stepped->built;
            step_record.released += stepped->released;
        }
        if (step % 7 == 0) {
            const VirtualItemId selection_key = range.key_at(step % kSelectionKeys);
            if (range.is_selected(selection_key)) {
                range.set_selected(selection_key, false);
            } else {
                range.set_selected(selection_key, true);
            }
        }
        step_record.selected = range.selected_items(selected);
        step_record.realized_first = *range.realized_index_at(0);
        step_record.realized_last = *range.realized_index_at(range.realized_count() - 1);
        result.steps.push_back(step_record);
    }
    result.build_order = std::move(source.build_order);
    result.release_order = std::move(source.release_order);
    result.focused_key = range.focused_key().value;
    result.selected_first = range.selected_items(selected) > 0 ? selected[0].value : 0;
    result.total_extent = range.geometry().total_extent();
    return result;
}

} // namespace

// Count allocations so the focus/selection path can be proven allocation-free after warm-up.
void* operator new(std::size_t size) {
    ++g_allocations;
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

int main() {
    ChurnSource source(false);
    Node container;
    VirtualRange range(churn_config(), source, container);
    if (!expect(range.sync().has_value(), "sync 10k items") ||
        !expect(range.item_count() == kItemCount, "item count")) {
        return 1;
    }

    // Warm the pool, the container's child vector, and the item-state store so the measured loop
    // below starts from a steady state. Every row is pinned to the minimum size first, and the
    // warm-up windows use an oversized viewport so their high-water mark is at least the widest
    // window the measured loop can ask for.
    for (std::size_t index = 0; index < kItemCount; ++index) {
        if (!expect(range.apply_measure(index, kMinMain).has_value(), "pin warm measurement")) {
            return 2;
        }
    }
    for (std::size_t step = 0; step < 16; ++step) {
        const float scroll = 1000.0f + static_cast<float>(step) * 1000.0f;
        if (!expect(range.update(scroll, kViewport * 2.0f).has_value(), "warm-up update") ||
            !expect(range.focus_item(range.key_at((step * 997) % kItemCount), kViewport * 2.0f)
                        .has_value(),
                    "warm-up focus") ||
            !expect(range.set_selected(range.key_at(step), true), "warm-up selection")) {
            return 2;
        }
    }
    range.clear_selection();
    const std::size_t baseline_allocations = g_allocations;
    const std::size_t baseline_nodes = range.recycler().node_count();
    std::array<VirtualItemId, 64> selected{};
    bool hot_ok = true;
    std::size_t focused_steps = 0;
    std::size_t selection_steps = 0;

    for (std::size_t step = 0; step < 800 && hot_ok; ++step) {
        const std::size_t index = (step * 37) % kItemCount;
        const auto focused = range.focus_item(range.key_at(index), kViewport);
        hot_ok = focused.has_value() && focused->realized;
        hot_ok = hot_ok && range.has_focus() && range.is_focused(range.key_at(index));
        hot_ok = hot_ok && range.focused_node() == range.realized_node(index);
        hot_ok = hot_ok && focus_bits(container) == 1;
        hot_ok = hot_ok && range.realized_count() <= kPoolCap &&
                 range.recycler().in_use_count() == range.realized_count();
        if (step % 4 == 0) {
            const auto stepped =
                range.step_focus(step % 8 == 0 ? FocusStep::Next : FocusStep::Previous, kViewport);
            hot_ok = hot_ok && stepped.has_value();
            hot_ok = hot_ok && focus_bits(container) == 1;
            ++focused_steps;
        }
        if (step % 7 == 0) {
            const VirtualItemId selection_key = range.key_at(step % kSelectionKeys);
            const bool on = range.is_selected(selection_key);
            hot_ok = hot_ok && range.set_selected(selection_key, !on);
            hot_ok = hot_ok && range.selected_items(selected) == range.selected_count();
            ++selection_steps;
        }
        if (step % 11 == 0) {
            hot_ok = hot_ok && range.step_focus(FocusStep::First, kViewport).has_value();
            hot_ok = hot_ok && range.step_focus(FocusStep::Last, kViewport).has_value();
            hot_ok = hot_ok && focus_bits(container) == 1;
        }
    }

    if (!expect(hot_ok, "focus/selection churn keeps one focused node inside the pool") ||
        !expect(g_allocations == baseline_allocations,
                "focus/selection churn performs no allocations after warm-up") ||
        !expect(range.recycler().node_count() == baseline_nodes &&
                    range.recycler().node_count() <= kPoolCap,
                "pool node count stays at its warm high-water mark") ||
        !expect(focused_steps > 0 && selection_steps > 0, "the script exercised both paths") ||
        !expect(range.selected_count() > 0 && range.has_focus(),
                "focus and selection are still active at the end")) {
        return 3;
    }

    // Releasing the whole window returns every node to the pool and clears the node mirrors; the
    // keyed focus survives without a realized node.
    const auto emptied = range.update(0.0f, 0.0f);
    if (!expect(emptied.has_value() && range.realized_count() == 0,
                "empty update realizes nothing") ||
        !expect(container.children().empty() && range.recycler().in_use_count() == 0,
                "no node is left in use") ||
        !expect(range.has_focus() && range.focused_node() == nullptr,
                "the keyed focus survives without a node")) {
        return 4;
    }

    // Determinism: the same script produces identical step records, build/release order, final
    // focus, first selected key in data order, and geometry.
    const ScriptResult first = run_script(300, true);
    const ScriptResult second = run_script(300, true);
    if (!expect(!first.steps.empty() && first.steps.size() == second.steps.size(),
                "both runs completed the script") ||
        !expect(first.steps == second.steps, "step records are identical") ||
        !expect(first.build_order == second.build_order, "build order is identical") ||
        !expect(first.release_order == second.release_order, "release order is identical") ||
        !expect(first.focused_key == second.focused_key, "final focused key is identical") ||
        !expect(first.selected_first == second.selected_first, "first selected key is identical") ||
        !expect(first.total_extent == second.total_extent, "final geometry is identical")) {
        return 5;
    }

    return expect(first.build_order.size() > 100 && first.release_order.size() > 100,
                  "determinism script exercised sustained churn")
               ? 0
               : 6;
}
