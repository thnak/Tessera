// SPDX-License-Identifier: MIT
//
// `VirtualRange::scroll_to_index` alignment and clamping
// (`docs/specs/virtualization-v1.md` sections 4 and 6): start/center/nearest placement, the
// [0, total - viewport] clamp, the pure-query (no-resync) contract, and the re-anchor interaction
// when measuring the newly realized window moves the scroll while focus stays keyed.

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/pseudo_state.hpp"
#include "tessera/ui/virtual/virtual_range.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <utility>
#include <vector>

namespace {

using tessera::ui::Node;
using tessera::ui::style::has_state;
using tessera::ui::style::PseudoState;
using tessera::ui::virtualization::MeasureConstraints;
using tessera::ui::virtualization::ScrollAlignment;
using tessera::ui::virtualization::VirtualDataSource;
using tessera::ui::virtualization::VirtualItemId;
using tessera::ui::virtualization::VirtualItemMeasure;
using tessera::ui::virtualization::VirtualRange;
using tessera::ui::virtualization::VirtualRangeError;

constexpr float kViewport = 100.0f;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "virtual_scroll_index: " << message << "\n";
    }
    return condition;
}

bool near(float value, float expected, float tolerance = 1.0e-3f) {
    return std::fabs(value - expected) <= tolerance;
}

// Fixed 32 px rows by default; the re-anchor case uses a row-varying size so measuring the
// overscanned window provably moves the scroll.
class SizedSource final : public VirtualDataSource {
public:
    SizedSource(std::size_t count, bool varying)
        : count_(count),
          varying_(varying) {}

    [[nodiscard]] std::size_t item_count() const override {
        return count_;
    }
    [[nodiscard]] VirtualItemId item_key(std::size_t index) const override {
        return VirtualItemId{9000 + index};
    }
    [[nodiscard]] VirtualItemMeasure measure(std::size_t index,
                                             const MeasureConstraints& constraints) const override {
        const float main = varying_ ? 32.0f + static_cast<float>(index % 3) * 8.0f : 32.0f;
        return VirtualItemMeasure{main, constraints.cross_extent};
    }
    void build(std::size_t, Node& out) override {
        out.set_element_kind("row");
        ++build_calls;
    }
    void release(std::size_t, Node&) noexcept override {
        ++release_calls;
    }

    std::size_t build_calls = 0;
    std::size_t release_calls = 0;

private:
    std::size_t count_ = 0;
    bool varying_ = false;
};

VirtualRange::Config config_for(std::size_t capacity, float overscan) {
    VirtualRange::Config config;
    config.capacity = capacity;
    config.pool_cap = 16;
    config.state_capacity = 32;
    config.estimate_main = 32.0f;
    config.gap = 0.0f;
    config.overscan_main = overscan;
    config.cross_extent = 100.0f;
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

} // namespace

int main() {
    SizedSource source(100, false);
    Node container;
    VirtualRange range(config_for(128, 0.0f), source, container);
    if (!expect(range.sync().has_value(), "sync 100 fixed rows")) {
        return 1;
    }

    // Start, center and nearest from a scroll of zero.
    auto target = range.scroll_to_index(10, kViewport, ScrollAlignment::Start);
    if (!expect(target.has_value() && target->index == 10, "Start resolves the index") ||
        !expect(near(target->scroll_offset, 320.0f), "Start puts the item at the viewport start")) {
        return 2;
    }
    target = range.scroll_to_index(10, kViewport, ScrollAlignment::Center);
    if (!expect(near(target->scroll_offset, 286.0f),
                "Center puts the item center at the viewport center")) {
        return 3;
    }
    // Nearest: an already-visible item does not move, an item below bottom-aligns.
    if (!expect(near(range.scroll_to_index(0, kViewport, ScrollAlignment::Nearest)->scroll_offset,
                     0.0f),
                "Nearest leaves a visible first item alone") ||
        !expect(near(range.scroll_to_index(4, kViewport, ScrollAlignment::Nearest)->scroll_offset,
                     60.0f),
                "Nearest bottom-aligns an item below the viewport") ||
        !expect(near(range.scroll_to_index(3, kViewport, ScrollAlignment::Nearest)->scroll_offset,
                     28.0f),
                "Nearest bottom-aligns a partially visible item below")) {
        return 4;
    }
    if (!expect(range.update(320.0f, kViewport).has_value(), "scroll to the index-10 window")) {
        return 5;
    }
    // Nearest is relative to the current scroll: no move inside the viewport, the shorter move
    // otherwise.
    if (!expect(near(range.scroll_to_index(10, kViewport, ScrollAlignment::Nearest)->scroll_offset,
                     320.0f),
                "Nearest keeps a fully visible item in place") ||
        !expect(near(range.scroll_to_index(12, kViewport, ScrollAlignment::Nearest)->scroll_offset,
                     320.0f),
                "Nearest keeps the last fully visible item in place") ||
        !expect(near(range.scroll_to_index(13, kViewport, ScrollAlignment::Nearest)->scroll_offset,
                     348.0f),
                "Nearest moves the minimum distance for an item below") ||
        !expect(near(range.scroll_to_index(5, kViewport, ScrollAlignment::Nearest)->scroll_offset,
                     160.0f),
                "Nearest top-aligns an item above the viewport")) {
        return 6;
    }
    if (!expect(near(range.scroll_to_index(12, kViewport, ScrollAlignment::Start)->scroll_offset,
                     384.0f),
                "Start ignores the current scroll") ||
        !expect(near(range.scroll_to_index(12, kViewport, ScrollAlignment::Center)->scroll_offset,
                     350.0f),
                "Center ignores the current scroll")) {
        return 7;
    }

    // Clamping: never before zero, never past total - viewport.
    if (!expect(near(range.scroll_to_index(99, kViewport, ScrollAlignment::Start)->scroll_offset,
                     3100.0f),
                "Start clamps at the scrollable end") ||
        !expect(near(range.scroll_to_index(99, kViewport, ScrollAlignment::Center)->scroll_offset,
                     3100.0f),
                "Center clamps at the scrollable end") ||
        !expect(near(range.scroll_to_index(99, kViewport, ScrollAlignment::Nearest)->scroll_offset,
                     3100.0f),
                "Nearest clamps at the scrollable end") ||
        !expect(
            near(range.scroll_to_index(0, kViewport, ScrollAlignment::Center)->scroll_offset, 0.0f),
            "Center clamps at zero") ||
        !expect(
            near(range.scroll_to_index(0, 4000.0f, ScrollAlignment::Start)->scroll_offset, 0.0f),
            "a viewport taller than the content clamps to zero")) {
        return 8;
    }

    // Errors: the index must be inside the last sync's snapshot.
    const auto past_end = range.scroll_to_index(100, kViewport, ScrollAlignment::Start);
    if (!expect(!past_end.has_value() && past_end.error() == VirtualRangeError::IndexOutOfRange,
                "past-end index is an error")) {
        return 9;
    }
    SizedSource empty_source(0, false);
    Node empty_container;
    VirtualRange empty(config_for(8, 0.0f), empty_source, empty_container);
    if (!expect(empty.sync().has_value(), "sync an empty range") ||
        !expect(!empty.scroll_to_index(0, kViewport, ScrollAlignment::Start).has_value(),
                "an empty range has no index to scroll to")) {
        return 10;
    }

    // Measured sizes feed the alignment math: pin the first ten rows at 50 px and the rest at 32.
    SizedSource measured_source(100, false);
    Node measured_container;
    VirtualRange measured(config_for(128, 0.0f), measured_source, measured_container);
    if (!expect(measured.sync().has_value(), "sync the measured range")) {
        return 11;
    }
    for (std::size_t index = 0; index < 10; ++index) {
        if (!expect(measured.apply_measure(index, 50.0f).has_value(), "pin a 50 px row")) {
            return 12;
        }
    }
    if (!expect(near(measured.scroll_to_index(10, kViewport, ScrollAlignment::Start)->scroll_offset,
                     500.0f),
                "Start follows the measured prefix") ||
        !expect(
            near(measured.scroll_to_index(10, kViewport, ScrollAlignment::Center)->scroll_offset,
                 466.0f),
            "Center follows the measured prefix") ||
        !expect(
            near(measured.scroll_to_index(10, kViewport, ScrollAlignment::Nearest)->scroll_offset,
                 432.0f),
            "Nearest follows the measured prefix")) {
        return 13;
    }

    // No full resync: the query is pure, so realized nodes, build/release counts, and the range's
    // own scroll offset are unchanged no matter how many targets are computed.
    const std::size_t builds_before = source.build_calls;
    const std::size_t releases_before = source.release_calls;
    std::vector<Node*> realized_before;
    for (std::size_t position = 0; position < range.realized_count(); ++position) {
        realized_before.push_back(range.realized_node(*range.realized_index_at(position)));
    }
    const float scroll_before = range.scroll_offset();
    bool pure = true;
    for (std::size_t index = 0; index < 100 && pure; ++index) {
        const auto query = range.scroll_to_index(index, kViewport, ScrollAlignment::Center);
        pure = query.has_value() && query->index == index;
    }
    if (!expect(pure, "every index resolves") ||
        !expect(near(range.scroll_offset(), scroll_before), "queries do not move the scroll") ||
        !expect(source.build_calls == builds_before && source.release_calls == releases_before,
                "queries never build or release a node") ||
        !expect(range.realized_count() == realized_before.size(), "queries keep the window") ||
        !expect(container.children().size() == realized_before.size(),
                "queries keep the container children")) {
        return 14;
    }
    for (std::size_t position = 0; position < range.realized_count(); ++position) {
        if (!expect(range.realized_node(*range.realized_index_at(position)) ==
                        realized_before[position],
                    "queries keep the same realized nodes")) {
            return 15;
        }
    }
    const auto realized_target = range.scroll_to_index(50, kViewport, ScrollAlignment::Start);
    if (!expect(realized_target.has_value(), "target for the realized window") ||
        !expect(range.update(realized_target->scroll_offset, kViewport).has_value(),
                "update realizes the target") ||
        !expect(range.realized_node(50) != nullptr, "scroll_to_index target is realized")) {
        return 16;
    }

    // Re-anchor with focus: measuring the overscanned window before the viewport anchor shifts the
    // scroll by the same delta, so the anchor keeps its viewport-relative position while the
    // focused key stays keyed and its node stays realized and focused.
    SizedSource varying_source(100, true);
    Node varying_container;
    VirtualRange varying(config_for(128, 32.0f), varying_source, varying_container);
    if (!expect(varying.sync().has_value(), "sync the varying-size range")) {
        return 17;
    }
    const auto varying_focus = varying.focus_item(varying.key_at(20), kViewport);
    if (!expect(varying_focus.has_value() && varying_focus->realized,
                "focus realizes the target row") ||
        !expect(near(varying_focus->scroll_offset, 580.0f),
                "the measured window re-anchors the scroll (572 + 8 px)") ||
        !expect(varying.has_focus() && varying.is_focused(varying.key_at(20)),
                "focus stays keyed across the re-anchor") ||
        !expect(focus_bits(varying_container) == 1 &&
                    varying.focused_node() == varying.realized_node(20),
                "exactly the focused row carries the focus bit")) {
        return 18;
    }
    // The anchor item (index 17, the first visible row) keeps its 28 px offset inside the viewport:
    // 544 - 572 before the measurement, 552 - 580 after it.
    if (!expect(varying.has_anchor() && varying.anchor().key == varying.key_at(17),
                "anchor is the first visible row") ||
        !expect(near(varying.anchor().offset_in_item, 28.0f), "anchor offset is preserved") ||
        !expect(near(varying.geometry().item_offset(17) - varying.scroll_offset(), -28.0f),
                "anchor keeps its viewport-relative position") ||
        !expect(near(varying.geometry().item_offset(20) - varying.scroll_offset(), 92.0f),
                "focused row moved by the measured deltas") ||
        !expect(near(varying.realized_node(20)->bounds().y, 92.0f),
                "focused node is placed from the re-anchored scroll") ||
        !expect(near(varying.realized_node(20)->bounds().height, 48.0f),
                "focused node uses its measured main size")) {
        return 19;
    }
    // Re-running the query with the settled scroll keeps the focused row visible: Nearest is the
    // documented follow-up when a measurement pushed the target (spec section 6), and it never
    // touches the keyed focus.
    const auto settled = varying.scroll_to_index(20, kViewport, ScrollAlignment::Nearest);
    if (!expect(settled.has_value(), "settled query resolves") ||
        !expect(near(settled->scroll_offset, 620.0f),
                "Nearest bottom-aligns the row from the re-anchored scroll") ||
        !expect(varying.update(settled->scroll_offset, kViewport).has_value(),
                "update with the settled offset") ||
        !expect(near(varying.scroll_offset(), 620.0f) && varying.is_focused(varying.key_at(20)),
                "the second pass keeps the focus and the offset")) {
        return 20;
    }
    if (!expect(near(varying.realized_node(20)->bounds().y, 52.0f),
                "focused row is fully visible after the settled pass") ||
        !expect(focus_bits(varying_container) == 1, "one focused node after the settle")) {
        return 21;
    }

    return 0;
}
