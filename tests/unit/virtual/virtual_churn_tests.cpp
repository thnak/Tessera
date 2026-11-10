// SPDX-License-Identifier: MIT

// 10k-item scroll churn (`docs/specs/virtualization-v1.md` sections 11 and 12): the bookkeeping
// performs no heap allocation after the pool is warm, node count stays inside `pool_cap`, and the
// same script produces identical build/release order and offsets twice.

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/virtual/virtual_range.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <numeric>
#include <string_view>
#include <utility>
#include <vector>

namespace {

std::size_t g_allocations = 0;

using tessera::ui::Node;
using tessera::ui::virtualization::MeasureConstraints;
using tessera::ui::virtualization::VirtualDataSource;
using tessera::ui::virtualization::VirtualItemId;
using tessera::ui::virtualization::VirtualItemMeasure;
using tessera::ui::virtualization::VirtualRange;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "virtual_churn: " << message << "\n";
    }
    return condition;
}

constexpr std::size_t kItemCount = 10000;
constexpr std::size_t kPoolCap = 32;
constexpr float kViewport = 240.0f;
constexpr float kOverscan = 200.0f;

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
        // Borrowed literal only: build stays allocation-free so the churn counter measures the
        // collection's bookkeeping, not the test source.
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
    config.estimate_main = 24.0f;
    config.gap = 4.0f;
    config.overscan_main = kOverscan;
    config.cross_extent = 200.0f;
    return config;
}

struct StepRecord {
    std::size_t window_first = 0;
    std::size_t window_last = 0;
    std::size_t realized_first = 0;
    std::size_t realized_last = 0;
    std::size_t built = 0;
    std::size_t released = 0;
    std::size_t measured = 0;
    std::size_t reused = 0;
    float scroll = 0.0f;

    bool operator==(const StepRecord&) const = default;
};

struct ScriptResult {
    std::vector<StepRecord> steps;
    std::vector<std::size_t> build_order;
    std::vector<std::size_t> release_order;
    std::vector<std::size_t> realized_order;
    float total_extent = 0.0f;
    float scroll = 0.0f;
};

ScriptResult run_script(std::size_t step_count) {
    ChurnSource source(true);
    Node container;
    VirtualRange range(churn_config(), source, container);
    ScriptResult result;
    if (!range.sync().has_value()) {
        return result;
    }
    result.steps.reserve(step_count);
    for (std::size_t step = 0; step < step_count; ++step) {
        // Jump around deterministically so the script exercises forward and backward churn.
        const float scroll = static_cast<float>((step * 37) % kItemCount) * 33.0f;
        const auto update = range.update(scroll, kViewport);
        if (!update.has_value()) {
            return result;
        }
        result.steps.push_back(StepRecord{update->window.first, update->window.last,
                                          update->realized.first, update->realized.last,
                                          update->built, update->released, update->measured,
                                          update->reused, update->scroll_offset});
        if (step % 7 == 0) {
            const auto applied = range.apply_measure((step * 17) % kItemCount, 30.0f);
            if (!applied.has_value()) {
                return result;
            }
        }
    }
    result.build_order = std::move(source.build_order);
    result.release_order = std::move(source.release_order);
    for (std::size_t position = 0; position < range.realized_count(); ++position) {
        result.realized_order.push_back(*range.realized_index_at(position));
    }
    result.total_extent = range.geometry().total_extent();
    result.scroll = range.scroll_offset();
    return result;
}

} // namespace

// Count allocations so the scroll path can be proven allocation-free after warm-up.
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

    // Warm the pool and the container's child vector to their high-water mark. Every item is
    // pinned to the minimum measured size first so the widest window this script can ever ask for
    // exists during warm-up, not during the measured loop.
    for (std::size_t index = 0; index < kItemCount; ++index) {
        if (!expect(range.apply_measure(index, 24.0f).has_value(), "pin warm measurement")) {
            return 2;
        }
    }
    for (std::size_t step = 0; step < 16; ++step) {
        const auto warm = range.update(1000.0f + static_cast<float>(step) * 1000.0f, kViewport);
        if (!expect(warm.has_value(), "warm-up update")) {
            return 2;
        }
    }
    const std::size_t baseline_allocations = g_allocations;
    const std::size_t baseline_nodes = range.recycler().node_count();
    bool hot_ok = true;
    std::size_t total_built = 0;
    std::size_t total_released = 0;

    for (std::size_t step = 0; step < 1500 && hot_ok; ++step) {
        const auto update = range.update(static_cast<float>(step) * 250.0f, kViewport);
        hot_ok = update.has_value();
        if (!hot_ok) {
            break;
        }
        total_built += update->built;
        total_released += update->released;
        hot_ok = range.realized_count() <= kPoolCap;
        hot_ok = hot_ok && range.recycler().in_use_count() == range.realized_count();
        hot_ok = hot_ok && range.recycler().node_count() <= kPoolCap;
        for (std::size_t position = 0; position < range.realized_count(); ++position) {
            const std::size_t index = *range.realized_index_at(position);
            const Node* node = range.realized_node(index);
            hot_ok = hot_ok && node != nullptr && node->parent() == &container;
            hot_ok = hot_ok && node->element_kind() == std::string_view{"row"};
        }
        if (step % 10 == 0) {
            const auto applied = range.apply_measure((step * 31) % kItemCount, 30.0f);
            hot_ok = hot_ok && applied.has_value();
        }
    }

    if (!expect(hot_ok, "churn stays within the pool and realizes valid nodes") ||
        !expect(g_allocations == baseline_allocations,
                "scroll churn performs no allocations after warm-up") ||
        !expect(range.recycler().node_count() == baseline_nodes &&
                    range.recycler().node_count() <= kPoolCap,
                "pool node count stays at its warm high-water mark") ||
        !expect(total_built > 0 && total_released > 0, "churn exercised build and release") ||
        !expect(range.geometry().measured_count() > 0, "measures were applied")) {
        return 3;
    }

    // Releasing the whole window returns every node to the free ring with no leak.
    const auto emptied = range.update(0.0f, 0.0f);
    if (!expect(emptied.has_value() && range.realized_count() == 0,
                "empty update realizes nothing") ||
        !expect(container.children().empty(), "container has no item children") ||
        !expect(range.recycler().in_use_count() == 0, "no lease is left in use") ||
        !expect(range.recycler().free_count() == range.recycler().node_count(),
                "every created node is free")) {
        return 4;
    }

    // Determinism: two runs of the same script produce identical step records, build/release
    // order, realized order, and final geometry.
    const ScriptResult first = run_script(400);
    const ScriptResult second = run_script(400);
    if (!expect(!first.steps.empty() && first.steps.size() == second.steps.size(),
                "both runs completed the script") ||
        !expect(first.steps == second.steps, "step records are identical") ||
        !expect(first.build_order == second.build_order, "build order is identical") ||
        !expect(first.release_order == second.release_order, "release order is identical") ||
        !expect(first.realized_order == second.realized_order, "realized order is identical") ||
        !expect(first.total_extent == second.total_extent && first.scroll == second.scroll,
                "final geometry and scroll are identical")) {
        return 5;
    }

    // A different order is not required by determinism, but the two runs must have actually
    // churned: guard against a no-op script passing vacuously.
    return expect(first.build_order.size() > 1000 && first.release_order.size() > 1000,
                  "determinism script exercised sustained churn")
               ? 0
               : 6;
}
