// SPDX-License-Identifier: MIT
//
// Keyed focus and selection over `VirtualRange` (`docs/specs/virtualization-v1.md` sections 2, 3, 7
// and 11): the focused key keeps its state while scrolled out and is restored on realize, focus
// moves by Tab/arrow stepping with exactly one realized focused node, selection is reported in data
// order, and both survive a data update that keeps the keys.

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/pseudo_state.hpp"
#include "tessera/ui/virtual/item_state_store.hpp"
#include "tessera/ui/virtual/virtual_range.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

namespace {

using tessera::ui::Node;
using tessera::ui::style::has_state;
using tessera::ui::style::PseudoState;
using tessera::ui::virtualization::FocusStep;
using tessera::ui::virtualization::ItemState;
using tessera::ui::virtualization::MeasureConstraints;
using tessera::ui::virtualization::ScrollAlignment;
using tessera::ui::virtualization::VirtualDataSource;
using tessera::ui::virtualization::VirtualItemId;
using tessera::ui::virtualization::VirtualItemMeasure;
using tessera::ui::virtualization::VirtualRange;
using tessera::ui::virtualization::VirtualRangeError;

constexpr float kMain = 32.0f;
constexpr float kViewport = 100.0f;
constexpr std::size_t kCount = 100;
constexpr std::uint64_t kBaseKey = 1000;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "virtual_focus: " << message << "\n";
    }
    return condition;
}

bool near(float value, float expected, float tolerance = 1.0e-3f) {
    return std::fabs(value - expected) <= tolerance;
}

// Mutable keys behind the read-only source contract, so a test can model a component applying a
// data update between two `sync` calls.
class RowSource final : public VirtualDataSource {
public:
    explicit RowSource(std::vector<std::uint64_t> initial_keys)
        : keys(std::move(initial_keys)) {}

    [[nodiscard]] std::size_t item_count() const override {
        return keys.size();
    }
    [[nodiscard]] VirtualItemId item_key(std::size_t index) const override {
        return VirtualItemId{keys[index]};
    }
    [[nodiscard]] VirtualItemMeasure measure(std::size_t,
                                             const MeasureConstraints& constraints) const override {
        return VirtualItemMeasure{kMain, constraints.cross_extent};
    }
    void build(std::size_t, Node& out) override {
        out.set_element_kind("row");
        ++build_calls;
    }

    std::vector<std::uint64_t> keys;
    std::size_t build_calls = 0;
};

VirtualRange::Config config_for(std::size_t capacity, std::size_t pool_cap,
                                std::size_t state_capacity = 64) {
    VirtualRange::Config config;
    config.capacity = capacity;
    config.pool_cap = pool_cap;
    config.state_capacity = state_capacity;
    config.estimate_main = kMain;
    config.gap = 0.0f;
    config.overscan_main = 0.0f;
    config.cross_extent = 100.0f;
    return config;
}

std::vector<std::uint64_t> sequential_keys(std::size_t count, std::uint64_t base) {
    std::vector<std::uint64_t> keys(count);
    std::iota(keys.begin(), keys.end(), base);
    return keys;
}

// Every realized item node is a child of the container, so counting Focus bits over the children
// also catches a bit left on a node that no longer belongs to the focused item.
std::size_t realized_focus_bits(const Node& container) {
    std::size_t count = 0;
    for (const auto& child : container.children()) {
        if (has_state(child->pseudo_state(), PseudoState::Focus)) {
            ++count;
        }
    }
    return count;
}

std::size_t count_selected(VirtualRange& range) {
    std::array<VirtualItemId, 32> buffer{};
    return range.selected_items(buffer);
}

} // namespace

int main() {
    RowSource source(sequential_keys(kCount, kBaseKey));
    Node container;
    VirtualRange range(config_for(128, 16), source, container);
    if (!expect(range.sync().has_value(), "sync 100 items") ||
        !expect(!range.has_focus() && range.focused_node() == nullptr, "no focus after sync")) {
        return 1;
    }

    // Focus is keyed: the state is written by VirtualItemId, not attached to the node.
    const VirtualItemId key = range.key_at(50);
    ItemState* state = range.item_state().find(key);
    if (!expect(state == nullptr, "no state for an unvisited key")) {
        return 2;
    }
    (void)range.item_state().put(key, ItemState{1, 0xF00DULL, kMain, 100.0f});
    auto update = range.update(0.0f, kViewport);
    if (!expect(update.has_value() && range.realized_count() == 4, "top window")) {
        return 3;
    }

    // focus_item realizes and scrolls to an unrealized key: index 50 is five viewports down.
    auto focused = range.focus_item(key, kViewport);
    if (!expect(focused.has_value(), "focus_item resolves a known key") ||
        !expect(focused->index == 50 && focused->key == key, "focus_item reports the index") ||
        !expect(focused->realized, "focus_item realized the target") ||
        !expect(near(focused->scroll_offset, 1532.0f), "Nearest bottom-aligns the target") ||
        !expect(range.has_focus() && range.is_focused(key) && range.focused_key() == key,
                "focus is stored by key") ||
        !expect(range.item_state().peek(key)->focused, "store mirrors the focus")) {
        return 4;
    }
    Node* focused_node = range.focused_node();
    if (!expect(focused_node == range.realized_node(50), "focused_node is the target node") ||
        !expect(has_state(focused_node->pseudo_state(), PseudoState::Focus),
                "focused node mirrors PseudoState::Focus") ||
        !expect(realized_focus_bits(container) == 1, "exactly one realized node is focused") ||
        !expect(near(focused_node->bounds().y, 68.0f), "target sits inside the viewport") ||
        !expect(range.item_state().peek(key)->payload == 0xF00DULL, "keyed payload is intact")) {
        return 5;
    }

    // Scrolling the focused item out keeps the keyed state and clears the node mirror; scrolling
    // back restores it on whichever node now shows the item.
    update = range.update(0.0f, kViewport);
    if (!expect(update.has_value(), "scroll back to the top") ||
        !expect(range.realized_node(50) == nullptr, "focused item is no longer realized") ||
        !expect(range.focused_node() == nullptr, "no realized focused node while scrolled out") ||
        !expect(range.has_focus() && range.is_focused(key), "focus survives scrolling out") ||
        !expect(realized_focus_bits(container) == 0, "no stale focus bit on recycled nodes") ||
        !expect(range.item_state().peek(key)->payload == 0xF00DULL, "state survives the recycle")) {
        return 6;
    }
    focused = range.focus_item(key, kViewport);
    if (!expect(focused.has_value() && focused->realized, "re-focus realizes the item again") ||
        !expect(range.focused_node() != nullptr && realized_focus_bits(container) == 1,
                "exactly one realized node is focused after the restore") ||
        !expect(has_state(range.focused_node()->pseudo_state(), PseudoState::Focus),
                "restored node carries the focus bit")) {
        return 7;
    }

    // Tab/arrow stepping: each step realizes the neighbour, keeps exactly one focused node, and
    // clamps at the collection ends instead of wrapping.
    auto stepped = range.step_focus(FocusStep::Next, kViewport);
    if (!expect(stepped.has_value() && stepped->index == 51, "Next steps one item down") ||
        !expect(stepped->realized && realized_focus_bits(container) == 1,
                "stepping keeps one realized focused node") ||
        !expect(range.is_focused(range.key_at(51)) && !range.is_focused(key),
                "focus moved off the previous key") ||
        !expect(near(stepped->scroll_offset, 1564.0f), "Nearest keeps the neighbour visible") ||
        !expect(near(range.realized_node(51)->bounds().y, 68.0f),
                "the stepped-to item is inside the viewport")) {
        return 8;
    }
    stepped = range.step_focus(FocusStep::First, kViewport);
    if (!expect(stepped.has_value() && stepped->index == 0 && stepped->realized,
                "First realizes the leading item") ||
        !expect(near(stepped->scroll_offset, 0.0f) && stepped->clamped == false,
                "First needs no clamp at the top") ||
        !expect(realized_focus_bits(container) == 1, "one focused node after First")) {
        return 9;
    }
    const auto previous_at_zero = range.step_focus(FocusStep::Previous, kViewport);
    if (!expect(previous_at_zero.has_value() && previous_at_zero->index == 0,
                "Previous clamps at the first item") ||
        !expect(range.has_focus() && range.is_focused(range.key_at(0)), "focus stays at index 0")) {
        return 10;
    }
    const auto last = range.step_focus(FocusStep::Last, kViewport);
    if (!expect(last.has_value() && last->index == 99 && last->realized,
                "Last realizes the final item") ||
        !expect(near(last->scroll_offset, 3100.0f), "Last bottom-aligns at the clamped end") ||
        !expect(realized_focus_bits(container) == 1 &&
                    has_state(range.focused_node()->pseudo_state(), PseudoState::Focus),
                "final item is the only focused node")) {
        return 11;
    }
    const auto next_at_end = range.step_focus(FocusStep::Next, kViewport);
    if (!expect(next_at_end.has_value() && next_at_end->index == 99,
                "Next clamps at the last item") ||
        !expect(realized_focus_bits(container) == 1, "clamped Next keeps one focus bit")) {
        return 12;
    }

    // With no focus yet, a step takes the first visible item instead of skipping it.
    RowSource fresh_source(sequential_keys(kCount, kBaseKey));
    Node fresh_container;
    VirtualRange fresh(config_for(128, 16), fresh_source, fresh_container);
    if (!expect(fresh.sync().has_value(), "sync for the no-focus step") ||
        !expect(fresh.update(0.0f, kViewport).has_value(), "top window for the no-focus step")) {
        return 13;
    }
    const auto first_step = fresh.step_focus(FocusStep::Next, kViewport);
    if (!expect(first_step.has_value() && first_step->index == 0,
                "Next without focus takes the first visible item") ||
        !expect(realized_focus_bits(fresh_container) == 1, "one focused node")) {
        return 14;
    }

    // Explicit focus/clear and the unknown-key errors.
    if (!expect(range.clear_focus(), "clear an active focus") ||
        !expect(!range.has_focus() && realized_focus_bits(container) == 0,
                "clearing focus clears the node mirror") ||
        !expect(!range.clear_focus(), "clearing absent focus reports false") ||
        !expect(!range.set_focus(VirtualItemId{424242}), "unknown key cannot be focused") ||
        !expect(!range.focus_item(VirtualItemId{424242}, kViewport).has_value(),
                "focus_item rejects an unknown key")) {
        return 15;
    }
    const auto unknown = range.focus_item(VirtualItemId{424242}, kViewport);
    if (!expect(unknown.error() == VirtualRangeError::UnknownKey, "unknown-key error code")) {
        return 16;
    }
    // Focus without scrolling only mirrors onto an already-realized node.
    Node* node7 = nullptr;
    if (!expect(range.update(224.0f, kViewport).has_value(), "scroll so index 7 is realized") ||
        !expect((node7 = range.realized_node(7)) != nullptr, "index 7 realized") ||
        !expect(range.set_focus(range.key_at(7)), "focus a realized key without scrolling") ||
        !expect(range.focused_node() == node7, "focus is mirrored on the realized node") ||
        !expect(near(range.scroll_offset(), 224.0f), "set_focus does not scroll") ||
        !expect(realized_focus_bits(container) == 1, "one focused node")) {
        return 17;
    }

    // Selection is reported in data order regardless of the order the keys were selected in.
    if (!expect(range.set_selected(range.key_at(99), true), "select the last key") ||
        !expect(range.set_selected(range.key_at(3), true), "select index 3") ||
        !expect(range.set_selected(range.key_at(50), true), "select index 50") ||
        !expect(range.selected_count() == 3, "three keys selected") ||
        !expect(range.is_selected(range.key_at(50)), "membership by key")) {
        return 18;
    }
    std::array<VirtualItemId, 8> selected{};
    const std::size_t selected_count = range.selected_items(selected);
    if (!expect(selected_count == 3, "selected_items counts every key") ||
        !expect(selected[0] == range.key_at(3) && selected[1] == range.key_at(50) &&
                    selected[2] == range.key_at(99),
                "selected_items follows data order") ||
        !expect(count_selected(range) == 3, "buffer query counts every selection")) {
        return 19;
    }
    if (!expect(!range.is_selected(range.key_at(7)), "unselected key is not a member") ||
        !expect(!range.set_selected(VirtualItemId{424242}, true),
                "unknown key cannot be selected") ||
        !expect(!range.toggle_selected(range.key_at(50)), "toggle removes index 50") ||
        !expect(range.selected_count() == 2, "toggle updates the count") ||
        !expect(range.toggle_selected(range.key_at(50)), "toggle restores index 50") ||
        !expect(range.clear_selection() && range.selected_count() == 0,
                "clear_selection empties the set") ||
        !expect(!range.clear_selection(), "clearing an empty selection reports false")) {
        return 20;
    }

    // A data update that keeps the keys keeps focus and selection; the ordered query follows the
    // new indices. Dropping a key drops its state on the next sync.
    if (!expect(range.set_focus(range.key_at(50)), "focus index 50") ||
        !expect(range.set_selected(range.key_at(99), true), "select index 99") ||
        !expect(range.set_selected(range.key_at(3), true), "select index 3") ||
        !expect(range.set_selected(range.key_at(50), true), "select index 50")) {
        return 21;
    }
    const VirtualItemId focused_before_update = range.focused_key();
    source.keys.insert(source.keys.begin(), kBaseKey - 1);
    if (!expect(range.sync().has_value(), "sync after an insert") ||
        !expect(range.item_count() == kCount + 1, "item count grew") ||
        !expect(range.realized_count() == 0, "sync releases realized nodes") ||
        !expect(range.has_focus() && range.focused_key() == focused_before_update,
                "focus survives a key-preserving data update") ||
        !expect(range.index_of(focused_before_update) == 51, "key moved to its new index") ||
        !expect(realized_focus_bits(container) == 0, "sync left no focus bit on a pooled node")) {
        return 22;
    }
    selected = {};
    if (!expect(range.selected_items(selected) == 3, "selection survives the data update") ||
        !expect(selected[0] == VirtualItemId{kBaseKey + 3} &&
                    selected[1] == VirtualItemId{kBaseKey + 50} &&
                    selected[2] == VirtualItemId{kBaseKey + 99},
                "selection order follows the shifted data indices")) {
        return 23;
    }
    update = range.update(0.0f, kViewport);
    if (!expect(update.has_value(), "update after the insert") ||
        !expect(range.focused_node() == nullptr,
                "the focused key is scrolled out at the top after the insert")) {
        return 24;
    }
    const auto refocused = range.focus_item(focused_before_update, kViewport);
    if (!expect(refocused.has_value() && refocused->index == 51 && refocused->realized,
                "focus_item follows the key to its new index") ||
        !expect(range.focused_node() == range.realized_node(51),
                "the node at the new index is focused") ||
        !expect(realized_focus_bits(container) == 1, "one focused node after the data update")) {
        return 25;
    }
    source.keys.erase(source.keys.begin() + 51); // the focused key itself disappears
    if (!expect(range.sync().has_value(), "sync after removing the focused key") ||
        !expect(!range.has_focus() && range.focused_key() == VirtualItemId{},
                "focus is dropped with its key") ||
        !expect(range.selected_count() == 2, "the other selections survive") ||
        !expect(range.selected_items(selected) == 2, "ordered selection still resolves")) {
        return 26;
    }

    // Pool-cap truncation: the target can be focused without being realized, and no node claims it.
    RowSource capped_source(sequential_keys(50, 5000));
    Node capped_container;
    VirtualRange capped(config_for(64, 2), capped_source, capped_container);
    if (!expect(capped.sync().has_value(), "sync for the capped range") ||
        !expect(capped.update(0.0f, kViewport).has_value(), "warm the capped range")) {
        return 27;
    }
    const auto capped_focus = capped.focus_item(capped.key_at(3), kViewport);
    if (!expect(capped_focus.has_value(), "capped focus resolves the key") ||
        !expect(capped_focus->truncated, "the pool cap truncates the window") ||
        !expect(!capped_focus->realized, "the truncated target is not realized") ||
        !expect(capped.has_focus() && capped.is_focused(capped.key_at(3)),
                "the key stays focused even when unrealized") ||
        !expect(capped.focused_node() == nullptr && realized_focus_bits(capped_container) == 0,
                "no node claims the unrealized focus")) {
        return 28;
    }

    // A zero-extent viewport focuses the key without realizing it; the next real update restores
    // it.
    RowSource zero_source(sequential_keys(20, 7000));
    Node zero_container;
    VirtualRange zero(config_for(32, 8), zero_source, zero_container);
    if (!expect(zero.sync().has_value(), "sync the zero-viewport range")) {
        return 29;
    }
    const auto zero_focus = zero.focus_item(zero.key_at(4), 0.0f);
    if (!expect(zero_focus.has_value(), "zero-viewport focus resolves the key") ||
        !expect(!zero_focus->realized && zero.realized_count() == 0,
                "zero viewport realizes nothing") ||
        !expect(zero.has_focus(), "zero viewport still stores the focus") ||
        !expect(zero.update(0.0f, kViewport).has_value() &&
                    zero.focused_node() == zero.realized_node(4),
                "the next update restores the focused node")) {
        return 30;
    }

    return 0;
}
