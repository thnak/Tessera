// SPDX-License-Identifier: MIT

// Contract tests for the read-only data source adapter: the collection only ever observes
// count/keys/measure, and `build` receives a clean node and writes item data into it.
// Recycler-driven cleaning is covered by `virtual_pool`.

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/virtual/virtual_data_source.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

using tessera::ui::Node;
using tessera::ui::virtualization::MeasureConstraints;
using tessera::ui::virtualization::VirtualDataSource;
using tessera::ui::virtualization::VirtualItemId;
using tessera::ui::virtualization::VirtualItemMeasure;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "virtual_data_source: " << message << "\n";
    }
    return condition;
}

// The data source is read-only: identity and measurement are const members, only `build`/`release`
// mutate the caller-owned node.
static_assert(std::is_same_v<decltype(&VirtualDataSource::item_count),
                             std::size_t (VirtualDataSource::*)() const>);
static_assert(std::is_same_v<decltype(&VirtualDataSource::item_key),
                             VirtualItemId (VirtualDataSource::*)(std::size_t) const>);
static_assert(std::is_same_v<decltype(&VirtualDataSource::measure),
                             VirtualItemMeasure (VirtualDataSource::*)(
                                 std::size_t, const MeasureConstraints&) const>);
static_assert(!std::is_copy_constructible_v<VirtualDataSource>,
              "a collection borrows one source; copying it would fork item identity");

class StaticSource final : public VirtualDataSource {
public:
    explicit StaticSource(std::size_t count)
        : count_(count) {}

    [[nodiscard]] std::size_t item_count() const override {
        ++count_calls;
        return count_;
    }
    [[nodiscard]] VirtualItemId item_key(std::size_t index) const override {
        return VirtualItemId{(index + 1) * 11};
    }
    [[nodiscard]] VirtualItemMeasure measure(std::size_t index,
                                             const MeasureConstraints& constraints) const override {
        ++measure_calls;
        if (constraints.cross_extent > 0.0f) {
            last_cross = constraints.cross_extent;
        }
        return VirtualItemMeasure{24.0f + static_cast<float>(index % 3) * 8.0f,
                                  constraints.cross_extent};
    }
    void build(std::size_t index, Node& out) override {
        ++build_calls;
        saw_element_state = !out.element_kind().empty() || !out.element_id().empty() ||
                            out.paint_data().has_value() || !out.children().empty() ||
                            out.scroll_y() != 0.0f || out.focusable();
        out.set_element_kind("row");
        out.set_element_id(row_ids[index % row_ids.size()]);
        tessera::ui::PaintData paint;
        paint.text = "row";
        out.set_paint_data(std::move(paint));
    }
    void release(std::size_t index, Node& out) noexcept override {
        ++release_calls;
        last_released_index = index;
        released_node = &out;
    }

    static constexpr std::array<std::string_view, 3> row_ids{"row-a", "row-b", "row-c"};

    mutable std::size_t count_calls = 0;
    mutable std::size_t measure_calls = 0;
    mutable float last_cross = 0.0f;
    std::size_t build_calls = 0;
    std::size_t release_calls = 0;
    std::size_t last_released_index = 0;
    Node* released_node = nullptr;
    // Set by build: true when the node handed in still carried item state.
    bool saw_element_state = false;

private:
    std::size_t count_ = 0;
};

} // namespace

int main() {
    StaticSource source(4);
    const VirtualDataSource& read_only = source;

    // count/key/measure are pure reads: repeated calls agree and only the counters move.
    const std::size_t count = read_only.item_count();
    const VirtualItemId key_one = read_only.item_key(1);
    const VirtualItemMeasure first = read_only.measure(1, MeasureConstraints{120.0f});
    const VirtualItemMeasure second = read_only.measure(1, MeasureConstraints{120.0f});
    if (!expect(count == 4, "source item count") ||
        !expect(key_one == VirtualItemId{22}, "stable key for index 1") ||
        !expect(first.main_size == 32.0f && first.cross_size == 120.0f,
                "measure applies the cross constraint") ||
        !expect(first.main_size == second.main_size && first.cross_size == second.cross_size,
                "measure is deterministic") ||
        !expect(read_only.item_key(0) == VirtualItemId{11}, "index 0 key")) {
        return 1;
    }

    // build writes a clean node and expects no prior item state; the released node is handed back
    // to the source before the recycler pools it.
    Node node(7);
    node.set_bounds(tessera::rendering::rhi::Rect{0.0f, 0.0f, 40.0f, 24.0f});
    source.build(1, node);
    if (!expect(!source.saw_element_state, "build starts from a clean node") ||
        !expect(node.element_kind() == "row", "build sets the element kind") ||
        !expect(node.element_id() == "row-b", "build sets the item id") ||
        !expect(node.paint_data().has_value(), "build sets item paint data")) {
        return 2;
    }
    source.release(1, node);
    if (!expect(source.release_calls == 1 && source.last_released_index == 1,
                "release is notified with the index") ||
        !expect(source.released_node == &node, "release receives the same node") ||
        !expect(node.element_id() == "row-b", "release does not reset the node itself")) {
        return 3;
    }

    // The default release hook is a no-op: a source that does not care cannot break pooling.
    struct SilentSource final : VirtualDataSource {
        [[nodiscard]] std::size_t item_count() const override {
            return 1;
        }
        [[nodiscard]] VirtualItemId item_key(std::size_t) const override {
            return VirtualItemId{1};
        }
        [[nodiscard]] VirtualItemMeasure measure(std::size_t,
                                                 const MeasureConstraints&) const override {
            return VirtualItemMeasure{32.0f, 100.0f};
        }
        void build(std::size_t, Node& out) override {
            out.set_element_kind("row");
        }
    } silent;
    Node silent_node;
    silent.build(0, silent_node);
    silent.release(0, silent_node);
    return expect(silent_node.element_kind() == "row", "default release leaves the node alone") ? 0
                                                                                                : 4;
}
