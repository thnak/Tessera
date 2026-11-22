// SPDX-License-Identifier: MIT

// The `NodeScroll`/`NodeMetadata` split (docs/architecture/god-type-audit-v1.md section 3.1, steps
// 2-3): the extracted types are exercised directly - not only through `Node` - so their own
// contracts are pinned and a later change cannot quietly move a rule back into the header.
//
// Two contracts matter and neither was stated anywhere before the split:
//   * a mutator reports the dirty domain its change implies, `Dirty::Clean` when it changed
//   nothing,
//     so the owner (and not the value type) decides what a frame owes;
//   * the scroll clamp reads the box it is told about instead of reaching into a node, so the
//     scrollport arithmetic is testable with no scene at all.
// No GPU, no wall clock.

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/node_scroll.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ui = tessera::ui;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

// `NodeMetadata` reports what each rule invalidates. The interesting rows are the ones that must
// NOT dirty: a kind, a role, an automation id and a verbatim inline-style rewrite paint nothing and
// re-resolve nothing, and a steady scene must stay clean through them.
void test_metadata_dirty_domains() {
    ui::NodeMetadata metadata;
    using ui::Dirty;

    check(metadata.set_element_kind("View") == Dirty::Clean,
          "an element kind is not a selector input, so it marks nothing");
    check(metadata.set_element_id("hero") == Dirty::Style, "an id re-resolves the cascade");
    check(metadata.set_element_id("hero") == Dirty::Clean, "re-setting the same id is a no-op");
    check(metadata.clear_element_id() == Dirty::Style, "clearing an id re-resolves the cascade");
    check(metadata.clear_element_id() == Dirty::Clean, "clearing an absent id is a no-op");

    check(metadata.set_class_name("card") == Dirty::Style, "a class re-resolves the cascade");
    check(metadata.set_class_name("card") == Dirty::Clean, "re-setting the same class is a no-op");
    check(metadata.clear_class_name() == Dirty::Style, "clearing a class re-resolves the cascade");

    check(metadata.set_part("label") == Dirty::Style, "a part name re-resolves ::part()");
    check(metadata.set_part("label") == Dirty::Clean, "re-setting the same part is a no-op");
    check(metadata.clear_part() == Dirty::Style, "clearing a part re-resolves ::part()");

    // A component name re-types the host; `Node` turns this answer into a subtree mark.
    check(metadata.set_component_name("Button") == Dirty::Style,
          "a component name re-resolves the host rules");
    check(metadata.set_component_name("Button") == Dirty::Clean,
          "re-setting the same component name is a no-op");
    check(metadata.clear_component_name() == Dirty::Style,
          "clearing a component name re-resolves the host rules");

    check(metadata.set_element_tag("button") == Dirty::Style, "an element tag is a selector input");
    check(metadata.set_element_tag("button") == Dirty::Clean, "re-setting the tag is a no-op");

    check(metadata.set_automation_id(7) == Dirty::Clean, "an automation id paints nothing");
    check(metadata.set_automation_id(7) == Dirty::Clean, "re-setting the automation id is a no-op");
    check(metadata.set_role("button") == Dirty::Clean, "a role paints nothing on its own");
    check(metadata.clear_role() == Dirty::Clean, "clearing a role paints nothing on its own");
}

// The inline-style write has to separate "accepted" from "changed": the cap and an empty name
// reject, and a verbatim rewrite is accepted but owes no frame.
void test_inline_style_write() {
    ui::NodeMetadata metadata;
    using ui::Dirty;

    check(!metadata.set_inline_style("", "1px").accepted, "an empty name is rejected");
    const ui::StyleWrite first = metadata.set_inline_style("width", "10px");
    check(first.accepted && first.dirty == Dirty::Style, "a new declaration dirties style");
    const ui::StyleWrite same = metadata.set_inline_style("width", "10px");
    check(same.accepted && same.dirty == Dirty::Clean,
          "re-stating a declaration verbatim is accepted and dirties nothing");
    const ui::StyleWrite changed = metadata.set_inline_style("width", "20px");
    check(changed.accepted && changed.dirty == Dirty::Style, "a changed value dirties style");

    // The names must outlive the metadata: a declaration stores a borrowed view, so a `std::string`
    // temporary per iteration would be dangling (and the table would silently dedupe by address).
    std::vector<std::string> names;
    names.reserve(ui::kMaxInlineStyleDeclarations + 1);
    for (std::size_t i = 1; i < ui::kMaxInlineStyleDeclarations; ++i) {
        names.push_back("p" + std::to_string(i));
        check(metadata.set_inline_style(names.back(), "1px").accepted,
              "declarations fill to the cap");
    }
    check(!metadata.set_inline_style("overflow", "1px").accepted, "the declaration cap rejects");
    check(metadata.clear_inline_style() == Dirty::Style, "clearing a used table dirties style");
    check(metadata.clear_inline_style() == Dirty::Clean, "clearing an empty table dirties nothing");
}

// The clamp is the scroll contract: per-axis bounds from the content size, the scrollport and the
// box, with a non-scrollable axis pinned at 0 and a non-finite request sanitized. The two
// scrollable axes are arguments, not fields: the flags byte stays on `Node`, which is what keeps
// this type 40 bytes and `sizeof(Node)` inside its budget.
void test_scroll_clamp() {
    ui::NodeScroll scroll;

    check(!scroll.set_scroll_offset(10.0f, 10.0f, 100.0f, 100.0f, false, false),
          "a non-scrollable axis clamps to 0, so nothing moves");
    check(scroll.scroll_x() == 0.0f && scroll.scroll_y() == 0.0f, "and the offsets stay at 0");

    scroll.set_content_size(300.0f, 900.0f);
    check(scroll.set_scroll_offset(50.0f, 100.0f, 100.0f, 100.0f, false, true),
          "a scrollable axis accepts an in-range offset");
    check(scroll.scroll_x() == 0.0f, "the non-scrollable x axis still clamps to 0");
    check(scroll.scroll_y() == 100.0f, "the y offset is stored");

    check(scroll.set_scroll_offset(0.0f, 10000.0f, 100.0f, 100.0f, false, true),
          "an over-range request moves");
    check(scroll.scroll_y() == 800.0f, "it clamps to content - viewport");
    check(!scroll.set_scroll_offset(0.0f, 800.0f, 100.0f, 100.0f, false, true),
          "re-setting the clamped offset reports no change");

    // Insets shrink the viewport, so the same content scrolls further.
    check(scroll.set_scrollport_insets(0.0f, 20.0f, 0.0f, 0.0f, 100.0f, 100.0f),
          "new insets are accepted");
    check(scroll.set_scroll_offset(0.0f, 1.0e9f, 100.0f, 100.0f, false, true),
          "the larger range is used");
    check(scroll.scroll_y() == 820.0f, "the clamp accounts for the scrollport insets");
    check(!scroll.set_scrollport_insets(0.0f, 20.0f, 0.0f, 0.0f, 100.0f, 100.0f),
          "re-stating the same insets reports no change");

    // Sanitizing: non-finite offsets and a non-positive box never produce a negative offset.
    scroll.set_scroll_offset(-5.0f, -5.0f, 100.0f, 100.0f, true, true);
    check(scroll.scroll_x() == 0.0f && scroll.scroll_y() == 0.0f, "a negative request clamps to 0");
    scroll.set_scroll_offset(std::numeric_limits<float>::quiet_NaN(), 10.0f, 100.0f, 100.0f, true,
                             true);
    check(scroll.scroll_x() == 0.0f, "a NaN request sanitizes to 0");
}

// A smooth-scroll target is policy state: the offset is not touched, and clearing resets the target
// to the stored offset. The "a target is active" bit stays on `Node`'s flags word (a separate bool
// on `NodeScroll` would pad the node past its budget), so `Node` is what exposes it.
void test_scroll_animation_target() {
    ui::NodeScroll scroll;
    scroll.set_animation_target(40.0f, 60.0f);
    check(scroll.animation_target_x() == 40.0f && scroll.animation_target_y() == 60.0f,
          "the target is stored");
    check(scroll.scroll_x() == 0.0f && scroll.scroll_y() == 0.0f,
          "setting a target moves no offset by itself");
    scroll.clear_animation();
    check(scroll.animation_target_x() == scroll.scroll_x(),
          "clearing resets the target to the stored offset");
    check(scroll.animation_target_y() == scroll.scroll_y(), "both axes reset to the stored offset");

    ui::Node node(1);
    check(!node.scroll_animation_active(), "no smooth-scroll target by default");
    node.set_scroll_animation_target(40.0f, 60.0f);
    check(node.scroll_animation_active(), "the node reports an active target");
    check(node.scroll_animation_target_y() == 60.0f, "the node reports the stored target");
    node.clear_scroll_animation();
    check(!node.scroll_animation_active(), "clearing the target stops it");
}

// The node-level forwarders have to keep the old dirty semantics exactly: this is the half a call
// site would notice if the split got it wrong.
void test_node_forwarders_keep_dirty_marks() {
    ui::Node node(1);
    node.add_child(std::make_unique<ui::Node>(2));
    node.children()[0]->clear_dirty(ui::Dirty::All);
    using ui::Dirty;

    node.clear_dirty(Dirty::All);
    node.set_class_name("card");
    check(node.dirty() == Dirty::Style, "a class still marks Style through the forwarder");
    node.clear_dirty(Dirty::All);

    node.set_element_kind("View");
    check(node.dirty() == Dirty::Clean, "an element kind still marks nothing");

    check(node.component_name().empty(), "a fresh node has no component name");
    node.set_component_name("Button");
    check(node.dirty() == Dirty::Style, "a component name marks this node");
    bool subtree_marked = !node.children().empty();
    for (const auto& child : node.children()) {
        subtree_marked = subtree_marked && child->subtree_dirty() == Dirty::Style;
        subtree_marked = subtree_marked && child->dirty() == Dirty::Style;
    }
    check(subtree_marked, "and every descendant, which is what the audit called out");

    ui::Node plain(2);
    plain.set_scrollable_y(true);
    check(plain.dirty() == (Dirty::Paint | Dirty::Render),
          "toggling a scrollable axis still marks Paint|Render");
}

} // namespace

int main() {
    test_metadata_dirty_domains();
    test_inline_style_write();
    test_scroll_clamp();
    test_scroll_animation_target();
    test_node_forwarders_keep_dirty_marks();

    if (failures != 0) {
        std::cerr << "node_views: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "node_views: ok\n";
    return 0;
}
