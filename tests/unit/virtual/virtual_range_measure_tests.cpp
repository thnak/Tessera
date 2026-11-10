// SPDX-License-Identifier: MIT

// Measure refinement against the scroll anchor (`docs/specs/virtualization-v1.md` section 6): an
// item measured before the viewport start shifts the scroll by the same delta, the anchor item
// keeps the offset, an item after the anchor changes nothing, and every result is clamped to the
// scrollable range.

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/virtual/scroll_anchor.hpp"
#include "tessera/ui/virtual/virtual_range.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <vector>

namespace {

using tessera::ui::Node;
using tessera::ui::virtualization::AnchorAdjustment;
using tessera::ui::virtualization::MeasureConstraints;
using tessera::ui::virtualization::reanchor_scroll;
using tessera::ui::virtualization::VirtualDataSource;
using tessera::ui::virtualization::VirtualItemId;
using tessera::ui::virtualization::VirtualItemMeasure;
using tessera::ui::virtualization::VirtualRange;
using tessera::ui::virtualization::VirtualRangeError;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "virtual_measure: " << message << "\n";
    }
    return condition;
}

bool near(float value, float expected, float tolerance = 1.0e-4f) {
    return std::fabs(value - expected) <= tolerance;
}

class FixedSource final : public VirtualDataSource {
public:
    explicit FixedSource(std::size_t count)
        : count_(count) {}

    [[nodiscard]] std::size_t item_count() const override {
        return count_;
    }
    [[nodiscard]] VirtualItemId item_key(std::size_t index) const override {
        return VirtualItemId{2000 + index};
    }
    [[nodiscard]] VirtualItemMeasure measure(std::size_t,
                                             const MeasureConstraints& constraints) const override {
        return VirtualItemMeasure{32.0f, constraints.cross_extent};
    }
    void build(std::size_t, Node& out) override {
        out.set_element_kind("row");
    }

private:
    std::size_t count_ = 0;
};

VirtualRange::Config anchor_config() {
    VirtualRange::Config config;
    config.capacity = 256;
    config.pool_cap = 32;
    config.estimate_main = 32.0f;
    config.gap = 0.0f;
    config.overscan_main = 100.0f;
    config.cross_extent = 100.0f;
    return config;
}

} // namespace

int main() {
    FixedSource source(200);
    Node container;
    VirtualRange range(anchor_config(), source, container);
    if (!expect(range.sync().has_value(), "sync")) {
        return 1;
    }

    // Scroll 320 px into 32 px items: visible items are 10..13, but the overscanned window starts
    // at 6, so measuring 6..9 must also move the anchor.
    auto update = range.update(320.0f, 100.0f);
    if (!update.has_value()) {
        return 2;
    }
    if (!expect(update->window.first == 6 && update->window.last == 17, "overscanned window") ||
        !expect(range.realized_count() == 11, "eleven realized items") ||
        !expect(range.has_anchor(), "anchor is set") ||
        !expect(range.anchor().key == VirtualItemId{2010}, "anchor key is the viewport start") ||
        !expect(near(range.anchor().offset_in_item, 0.0f), "anchor offset within the item")) {
        return 3;
    }

    // An item before the anchor shifts the scroll by the measured delta.
    const auto forward = range.apply_measure(5, 64.0f);
    if (!expect(forward.has_value(), "measure before the anchor") ||
        !expect(forward->shifted, "shift is reported") ||
        !expect(near(forward->scroll_offset, 352.0f), "scroll shifts by +32") ||
        !expect(near(range.scroll_offset(), 352.0f), "range keeps the shifted scroll")) {
        return 4;
    }
    const AnchorAdjustment oracle = reanchor_scroll(320.0f, 100.0f, 6432.0f, 10, 5, 32.0f);
    if (!expect(near(oracle.scroll_offset, forward->scroll_offset), "matches the re-anchor rule")) {
        return 5;
    }

    // An item after the anchor changes nothing, even though it changes the total extent.
    const auto after = range.apply_measure(20, 64.0f);
    if (!expect(after.has_value(), "measure after the anchor") ||
        !expect(!after->shifted, "no shift for an item after the anchor") ||
        !expect(near(after->scroll_offset, 352.0f), "scroll unchanged after the anchor")) {
        return 6;
    }

    // The anchor item itself changed size: keep the scroll (clamped to the valid range).
    const auto anchor_item = range.apply_measure(10, 64.0f);
    if (!expect(anchor_item.has_value(), "measure the anchor item") ||
        !expect(!anchor_item->shifted, "anchor item does not shift the scroll") ||
        !expect(near(anchor_item->scroll_offset, 352.0f), "scroll is kept")) {
        return 7;
    }
    // The realized node follows the refinement immediately: size and content-space position.
    const Node* anchor_node = range.realized_node(10);
    if (!expect(anchor_node != nullptr, "anchor node realized") ||
        !expect(near(anchor_node->bounds().height, 64.0f), "node main size follows the measure") ||
        !expect(near(anchor_node->bounds().y, 0.0f), "node position follows the refined offset")) {
        return 8;
    }

    // Shrinking an item before the anchor shifts the scroll back by the difference.
    const auto shrink = range.apply_measure(3, 16.0f);
    if (!expect(shrink.has_value() && shrink->shifted, "negative delta shifts back") ||
        !expect(near(shrink->scroll_offset, 336.0f), "scroll shifts by -16") ||
        !expect(near(range.scroll_offset(), 336.0f), "range keeps the negative shift")) {
        return 9;
    }

    // Measuring the same size again is not a change and leaves the scroll untouched.
    const auto repeat = range.apply_measure(10, 64.0f);
    if (!expect(repeat.has_value() && !repeat->shifted, "repeat measure is a no-op") ||
        !expect(near(repeat->scroll_offset, 336.0f), "repeat measure keeps the scroll")) {
        return 10;
    }

    // Out-of-range indices are rejected instead of silently clamping to the last item.
    const auto bad = range.apply_measure(400, 1.0f);
    if (!expect(!bad.has_value() && bad.error() == VirtualRangeError::IndexOutOfRange,
                "out-of-range measure is an error")) {
        return 11;
    }

    // Clamping: at the end of the list, shrinking an item after the anchor lowers the maximum
    // scroll and the stored offset is pulled back into range.
    FixedSource clamp_source(200);
    Node clamp_container;
    VirtualRange clamped(anchor_config(), clamp_source, clamp_container);
    if (!expect(clamped.sync().has_value(), "clamp sync")) {
        return 12;
    }
    const auto at_end = clamped.update(1.0e6f, 100.0f);
    if (!at_end.has_value()) {
        return 13;
    }
    if (!expect(at_end->clamped, "past-end update clamps") ||
        !expect(near(at_end->scroll_offset, 6300.0f), "end scroll is total minus viewport") ||
        !expect(clamped.anchor().key == VirtualItemId{2196}, "anchor at the tail")) {
        return 14;
    }
    const auto pull_back = clamped.apply_measure(199, 0.0f);
    if (!expect(pull_back.has_value(), "tail measure") ||
        !expect(pull_back->clamped && !pull_back->shifted, "clamp is reported, no shift") ||
        !expect(near(pull_back->scroll_offset, 6268.0f), "scroll clamped to the new maximum")) {
        return 15;
    }

    return 0;
}
