// SPDX-License-Identifier: MIT

// Visible-range integration over the prefix-sum geometry: realize/release churn, node reuse,
// pool-cap truncation, key resolution, sync after a data change, and the "build never sees
// recycled state" rule.

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/virtual/virtual_range.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using tessera::ui::Node;
using tessera::ui::virtualization::MeasureConstraints;
using tessera::ui::virtualization::node_carries_item_state;
using tessera::ui::virtualization::VirtualDataSource;
using tessera::ui::virtualization::VirtualItemId;
using tessera::ui::virtualization::VirtualItemMeasure;
using tessera::ui::virtualization::VirtualRange;
using tessera::ui::virtualization::VirtualRangeError;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "virtual_range: " << message << "\n";
    }
    return condition;
}

std::vector<std::uint64_t> sequential_keys(std::size_t count, std::uint64_t base) {
    std::vector<std::uint64_t> keys(count);
    std::iota(keys.begin(), keys.end(), base);
    return keys;
}

class ListSource final : public VirtualDataSource {
public:
    ListSource(std::vector<std::uint64_t> keys, float main_size)
        : keys_(std::move(keys)),
          main_(main_size) {}

    [[nodiscard]] std::size_t item_count() const override {
        return keys_.size();
    }
    [[nodiscard]] VirtualItemId item_key(std::size_t index) const override {
        return VirtualItemId{keys_[index]};
    }
    [[nodiscard]] VirtualItemMeasure measure(std::size_t,
                                             const MeasureConstraints& constraints) const override {
        ++measure_calls;
        return VirtualItemMeasure{main_, constraints.cross_extent};
    }
    void build(std::size_t index, Node& out) override {
        if (node_carries_item_state(out)) {
            ++dirty_inputs;
        }
        out.set_element_kind("row");
        out.set_element_id(ids[index % ids.size()]);
        ++build_calls;
    }
    void release(std::size_t, Node&) noexcept override {
        ++release_calls;
    }

    static constexpr std::array<std::string_view, 3> ids{"row-a", "row-b", "row-c"};

    mutable std::size_t measure_calls = 0;
    std::size_t build_calls = 0;
    std::size_t release_calls = 0;
    std::size_t dirty_inputs = 0;

private:
    std::vector<std::uint64_t> keys_;
    float main_ = 32.0f;
};

VirtualRange::Config vertical_config(std::size_t capacity, std::size_t pool_cap) {
    VirtualRange::Config config;
    config.capacity = capacity;
    config.pool_cap = pool_cap;
    config.estimate_main = 32.0f;
    config.gap = 0.0f;
    config.overscan_main = 0.0f;
    config.cross_extent = 100.0f;
    return config;
}

bool check_window(const VirtualRange::UpdateResult& update, std::size_t first, std::size_t last) {
    return update.window.first == first && update.window.last == last;
}

} // namespace

int main() {
    // Realize at the top, scroll forward (release then build), scroll back (reuse), and end at the
    // clamped tail. The data source never sees item state on the node it is asked to build.
    ListSource source(sequential_keys(100, 1000), 32.0f);
    Node container;
    VirtualRange range(vertical_config(128, 16), source, container);
    if (!expect(range.sync().has_value(), "sync a 100-item source") ||
        !expect(range.item_count() == 100, "item count after sync")) {
        return 1;
    }

    auto update = range.update(0.0f, 100.0f);
    if (!update.has_value()) {
        return 2;
    }
    if (!expect(check_window(*update, 0, 4), "top window") ||
        !expect(update->built == 4 && update->released == 0, "four items built") ||
        !expect(update->measured == 4 && update->reused == 0, "fresh nodes are allocations") ||
        !expect(range.realized_count() == 4, "four realized nodes") ||
        !expect(container.children().size() == 4, "container owns the realized nodes")) {
        return 3;
    }
    if (!expect(range.realized_node(0) != nullptr && range.realized_node(3) != nullptr,
                "first and last top items are realized") ||
        !expect(range.realized_node(4) == nullptr, "item past the window is not realized") ||
        !expect(range.realized_node(0)->bounds().y == 0.0f, "first item sits at the top") ||
        !expect(range.realized_node(3)->bounds().y == 96.0f, "last item is offset by the stride") ||
        !expect(range.realized_node(0)->bounds().height == 32.0f, "main size from measure") ||
        !expect(range.realized_node(0)->bounds().width == 100.0f, "cross size from constraints") ||
        !expect(container.content_height() == 3200.0f, "container content is the total extent")) {
        return 4;
    }

    update = range.update(64.0f, 100.0f);
    if (!update.has_value()) {
        return 5;
    }
    if (!expect(check_window(*update, 2, 6), "window after two strides") ||
        !expect(update->built == 2 && update->released == 2, "two built, two released") ||
        !expect(update->reused == 2, "freed nodes are reused") ||
        !expect(range.realized_node(0) == nullptr && range.realized_node(2) != nullptr,
                "leaving items are released and entering items realized") ||
        !expect(range.realized_node(2)->bounds().y == 0.0f, "node 2 is at the viewport top") ||
        !expect(source.dirty_inputs == 0, "build never sees recycled state")) {
        return 6;
    }

    update = range.update(0.0f, 100.0f);
    if (!update.has_value()) {
        return 7;
    }
    if (!expect(check_window(*update, 0, 4), "scrolled back to the top") ||
        !expect(update->built == 2 && update->released == 2, "backward churn counts") ||
        !expect(range.realized_node(0) != nullptr && range.realized_node(4) == nullptr,
                "backward scroll realizes the top items") ||
        !expect(source.dirty_inputs == 0, "reused nodes were cleaned before build")) {
        return 8;
    }

    // Past the end: the request clamps to the last full viewport and the tail is realized.
    update = range.update(1.0e6f, 100.0f);
    if (!update.has_value()) {
        return 9;
    }
    if (!expect(update->clamped, "past-end scroll is clamped") ||
        !expect(update->scroll_offset == 3100.0f, "clamped to total minus viewport") ||
        !expect(check_window(*update, 96, 100), "tail window") ||
        !expect(range.realized_node(99) != nullptr, "last item realized")) {
        return 10;
    }

    // A zero-extent viewport realizes nothing and returns every node to the pool.
    update = range.update(0.0f, 0.0f);
    if (!update.has_value()) {
        return 11;
    }
    if (!expect(update->window.empty() && update->released == 4, "empty viewport releases all") ||
        !expect(range.realized_count() == 0 && container.children().empty(),
                "no children remain after the empty update") ||
        !expect(range.recycler().in_use_count() == 0, "pool reports no lease in use")) {
        return 12;
    }

    // Pool cap: the window is wider than the pool, so the realized set truncates without mutating
    // the pool on a repeated update.
    ListSource wide_source(sequential_keys(50, 5000), 32.0f);
    Node wide_container;
    VirtualRange capped(vertical_config(64, 2), wide_source, wide_container);
    if (!expect(capped.sync().has_value(), "capped sync")) {
        return 13;
    }
    update = capped.update(0.0f, 100.0f);
    if (!update.has_value()) {
        return 14;
    }
    if (!expect(update->truncated, "window wider than the pool truncates") ||
        !expect(capped.realized_count() == 2 && update->realized.first == 0 &&
                    update->realized.last == 2,
                "realized prefix is reported") ||
        !expect(wide_container.children().size() == 2, "only two nodes attached") ||
        !expect(capped.recycler().node_count() == 2 && capped.recycler().stats().denied >= 1,
                "cap is a refusal, not growth")) {
        return 15;
    }
    const std::size_t denied_before = capped.recycler().stats().denied;
    update = capped.update(0.0f, 100.0f);
    if (!update.has_value()) {
        return 16;
    }
    if (!expect(update->built == 0 && update->released == 0, "repeat update is a no-op") ||
        !expect(capped.realized_count() == 2 && capped.recycler().in_use_count() == 2,
                "repeat update keeps the realized set") ||
        !expect(capped.recycler().node_count() == 2, "repeat update does not create nodes") ||
        !expect(capped.recycler().stats().denied == denied_before + 1,
                "the capped tail is refused again, not evicted")) {
        return 17;
    }

    // Horizontal orientation: main axis is x, cross axis is y.
    ListSource row_source(sequential_keys(5, 7000), 40.0f);
    Node row_container;
    VirtualRange::Config horizontal = vertical_config(16, 8);
    horizontal.estimate_main = 40.0f;
    horizontal.gap = 10.0f;
    horizontal.cross_extent = 50.0f;
    horizontal.horizontal = true;
    VirtualRange rows(horizontal, row_source, row_container);
    if (!expect(rows.sync().has_value(), "horizontal sync")) {
        return 18;
    }
    update = rows.update(0.0f, 100.0f);
    if (!update.has_value()) {
        return 19;
    }
    if (!expect(check_window(*update, 0, 3), "horizontal window") ||
        !expect(rows.realized_node(1)->bounds().x == 50.0f, "horizontal offset on x") ||
        !expect(rows.realized_node(1)->bounds().width == 40.0f, "horizontal main size") ||
        !expect(rows.realized_node(1)->bounds().height == 50.0f, "horizontal cross size") ||
        !expect(row_container.content_width() == 240.0f, "horizontal content width") ||
        !expect(row_container.content_height() == 50.0f, "horizontal content height")) {
        return 20;
    }

    // Key resolution and scroll-to-key, then a data change that inserts a key before everything
    // else: the key keeps resolving, the index moves, and realized nodes are dropped by sync.
    ListSource key_source(sequential_keys(8, 1000), 32.0f);
    Node key_container;
    VirtualRange keyed(vertical_config(32, 8), key_source, key_container);
    if (!expect(keyed.sync().has_value(), "keyed sync") ||
        !expect(keyed.index_of(VirtualItemId{1003}) == 3, "key resolves to index") ||
        !expect(keyed.key_at(3) == VirtualItemId{1003}, "index resolves to key") ||
        !expect(!keyed.index_of(VirtualItemId{424242}).has_value(), "unknown key misses")) {
        return 21;
    }
    const auto target = keyed.scroll_to_key(VirtualItemId{1003}, 100.0f);
    if (!expect(target.has_value() && target->index == 3, "scroll target index") ||
        !expect(target->scroll_offset == 96.0f, "scroll target offset")) {
        return 22;
    }
    const auto aligned = keyed.scroll_to_key(VirtualItemId{1003}, 100.0f, 1.0f);
    if (!expect(aligned.has_value() && aligned->scroll_offset == 0.0f,
                "align clamps at the start")) {
        return 23;
    }
    const auto unknown_target = keyed.scroll_to_key(VirtualItemId{999}, 100.0f);
    if (!expect(!unknown_target.has_value() &&
                    unknown_target.error() == VirtualRangeError::UnknownKey,
                "scroll to an unknown key is an error")) {
        return 24;
    }

    update = keyed.update(0.0f, 100.0f);
    if (!expect(update.has_value() && keyed.realized_count() == 4, "keyed update realized")) {
        return 25;
    }
    std::vector<std::uint64_t> inserted = sequential_keys(9, 999);
    ListSource inserted_source(std::move(inserted), 32.0f);
    // Swap the source behind the range: the test range owns a reference, so build a new range
    // over the inserted data to model the component's data-source update.
    Node inserted_container;
    VirtualRange inserted_range(vertical_config(32, 8), inserted_source, inserted_container);
    if (!expect(inserted_range.sync().has_value(), "sync inserted source") ||
        !expect(inserted_range.index_of(VirtualItemId{1003}) == 4, "inserted key shifts index") ||
        !expect(inserted_range.item_count() == 9, "inserted count")) {
        return 26;
    }
    if (!expect(keyed.sync().has_value(), "re-sync the original range") ||
        !expect(keyed.realized_count() == 0 && key_container.children().empty(),
                "sync drops realized nodes because indices moved") ||
        !expect(keyed.recycler().in_use_count() == 0, "sync returns nodes to the pool")) {
        return 27;
    }

    // Error paths: capacity overflow and duplicate keys invalidate the range instead of serving a
    // half-built mapping.
    ListSource too_many(sequential_keys(10, 1), 32.0f);
    Node error_container;
    VirtualRange overflow(vertical_config(4, 4), too_many, error_container);
    const auto overflow_result = overflow.sync();
    if (!expect(!overflow_result.has_value() &&
                    overflow_result.error() == VirtualRangeError::CapacityExceeded,
                "capacity overflow is reported") ||
        !expect(overflow.item_count() == 0, "overflow leaves an empty range")) {
        return 28;
    }
    ListSource duplicate({1, 2, 3, 2}, 32.0f);
    Node duplicate_container;
    VirtualRange duplicated(vertical_config(8, 4), duplicate, duplicate_container);
    const auto duplicate_result = duplicated.sync();
    if (!expect(!duplicate_result.has_value() &&
                    duplicate_result.error() == VirtualRangeError::DuplicateKey,
                "duplicate key is reported") ||
        !expect(duplicated.update(0.0f, 100.0f)->window.empty(),
                "duplicate key leaves an empty window")) {
        return 29;
    }

    return expect(source.build_calls > 0 && source.release_calls > 0,
                  "range exercised build and release") &&
                   expect(source.dirty_inputs == 0, "no build ever saw recycled state")
               ? 0
               : 30;
}
