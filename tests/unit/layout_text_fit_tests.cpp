// SPDX-License-Identifier: MIT
//
// Layout's text-fit fact (issue #490, docs/design/dirty-rendering.md section 6.8): the layout
// engine reports whether a node's plain-text payload provably stays inside the content box the
// paint pass will use, so the paint-pass cull can stop refusing every text node. The fact is only
// sound if it is computed from paint's own inputs, which is what this file pins:
//   - the content box is the border box minus the style's `padding-*` longhands, NOT the border box
//     minus padding AND border (`LayoutBox::content_insets`), because `paint_text_lines` subtracts
//     only the padding longhands;
//   - the wrap decision is paint's (`PaintData::wrap` opted in AND `white-space` wraps), carried on
//     `LayoutInput::text_wrap`;
//   - the measure is the engine's own text measure cache, so layout and paint agree on the metrics.
// Everything this file cannot prove stays `Unknown`, which paint reads as "not cullable".
// Deterministic: the stub measurer, hand-built style trees, no clock, font, network or GPU.

#include "tessera/ui/layout/apply_layout.hpp"
#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace layout_text_fit_test {

using tessera::ui::Node;
using tessera::ui::layout::apply_layout;
using tessera::ui::layout::LayoutBox;
using tessera::ui::layout::LayoutEngine;
using tessera::ui::layout::LayoutInput;
using tessera::ui::layout::LayoutResult;
using tessera::ui::layout::LayoutViewport;
using tessera::ui::layout::TextFit;
using tessera::ui::style::ComputedStyle;

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layout_text_fit: " << message << "\n";
    }
    return condition;
}

ComputedStyle style(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle result;
    for (const auto& [property, value] : values) {
        result.set(property, value);
    }
    return result;
}

// A column flex root as wide as the viewport, so a child box is exactly the size it asks for.
LayoutInput root(std::vector<LayoutInput> children) {
    LayoutInput input;
    input.id = 1;
    input.style = style({{"display", "flex"},
                         {"flex-direction", "column"},
                         {"align-items", "flex-start"},
                         {"width", "400px"},
                         {"height", "400px"}});
    input.children = std::move(children);
    return input;
}

// The stub measurer advances 0.5 em per code point, so a 16 px run is 8 px per character and one
// line is 19.2 px tall (ascent 0.8 em + descent 0.2 em + line gap 0.2 em).
LayoutInput text_leaf(std::uint64_t id, std::string text, bool wrap, const char* width,
                      const char* height) {
    LayoutInput leaf;
    leaf.id = id;
    leaf.text = std::move(text);
    leaf.text_wrap = wrap;
    leaf.style = style({{"width", width},
                        {"height", height},
                        {"box-sizing", "border-box"},
                        {"font-size", "16px"}});
    return leaf;
}

const LayoutBox* find(const LayoutResult& result, std::uint64_t id) {
    for (const LayoutBox& box : result.boxes) {
        if (box.id == id) {
            return &box;
        }
    }
    return nullptr;
}

TextFit fit_of(const LayoutEngine& engine, const LayoutInput& scene, std::uint64_t node = 2) {
    const LayoutResult result = engine.layout(scene, LayoutViewport{400.0f, 400.0f});
    const LayoutBox* box = find(result, node);
    return box != nullptr ? box->text_fit : TextFit::Unknown;
}

// --- Fits -------------------------------------------------------------------------------------

bool test_text_that_fits_is_proved() {
    const LayoutEngine engine;
    // "hi" is 2 * 8 = 16 px wide and one line tall in a 120x24 box.
    const LayoutInput scene = root({text_leaf(2, "hi", false, "120px", "24px")});
    return expect(fit_of(engine, scene, 2) == TextFit::Fits, "short text in a roomy box fits");
}

bool test_wrapped_text_that_still_fits_is_proved() {
    const LayoutEngine engine;
    // Wrapped at the 120 px content width: "one two" is 7 * 8 = 56 px, so two lines fit in 48 px.
    const LayoutInput scene = root({text_leaf(2, "one two", true, "120px", "48px")});
    return expect(fit_of(engine, scene, 2) == TextFit::Fits,
                  "wrapped text whose measured height fits the box is proved");
}

// --- Overflows --------------------------------------------------------------------------------

bool test_text_taller_than_the_content_box_overflows() {
    const LayoutEngine engine;
    // The same wrapped run in a 20 px box: the measure reports 2 lines (38.4 px), which cannot fit.
    const LayoutInput scene = root({text_leaf(2, "one two three four five", true, "80px", "20px")});
    return expect(fit_of(engine, scene, 2) == TextFit::Overflows,
                  "wrapped text taller than the content box overflows");
}

bool test_text_wider_than_the_content_box_overflows() {
    const LayoutEngine engine;
    // No wrap: one 10-character line is 80 px and the box is 40 px wide.
    const LayoutInput scene = root({text_leaf(2, "abcdefghij", false, "40px", "40px")});
    return expect(fit_of(engine, scene, 2) == TextFit::Overflows,
                  "unwrapped text wider than the content box overflows");
}

bool test_the_wrap_opt_in_decides_the_result() {
    const LayoutEngine engine;
    // The same text and box, once unwrapped (80 px wide line, overflows) and once wrapped (two 40
    // px lines inside a 48 px box, fits). This is what makes `LayoutInput::text_wrap` load-bearing:
    // the fact must use paint's decision, not a default.
    const LayoutInput unwrapped = root({text_leaf(2, "abcdefghij", false, "40px", "48px")});
    const LayoutInput wrapped = root({text_leaf(2, "abcdefghij", true, "40px", "48px")});
    return expect(fit_of(engine, unwrapped, 2) == TextFit::Overflows,
                  "without the wrap opt-in the line stays wider than the box") &&
           expect(fit_of(engine, wrapped, 2) == TextFit::Fits,
                  "with the wrap opt-in the same run fits");
}

bool test_a_non_wrapping_white_space_suppresses_the_wrap() {
    const LayoutEngine engine;
    // `text_wrap` is paint's `PaintWrap::Wrap` opt-in; a computed `white-space: nowrap` still wins,
    // so the fact must stay unbounded even though the flag is set and the box would fit a wrapped
    // run.
    LayoutInput leaf = text_leaf(2, "abcdefghij", true, "40px", "48px");
    leaf.style.set("white-space", "nowrap");
    const LayoutInput scene = root({std::move(leaf)});
    return expect(fit_of(engine, scene, 2) == TextFit::Overflows,
                  "white-space: nowrap suppresses paint's wrap even with the opt-in");
}

// --- Unknown ----------------------------------------------------------------------------------

bool test_a_node_without_text_is_unknown() {
    const LayoutEngine engine;
    LayoutInput plain;
    plain.id = 2;
    plain.style = style({{"width", "120px"}, {"height", "24px"}, {"box-sizing", "border-box"}});
    const LayoutInput scene = root({std::move(plain)});
    return expect(fit_of(engine, scene, 2) == TextFit::Unknown, "a non-text node is unknown");
}

bool test_a_display_none_subtree_is_unknown() {
    const LayoutEngine engine;
    LayoutInput container;
    container.id = 2;
    container.style = style({{"display", "none"}, {"width", "120px"}, {"height", "24px"}});
    container.children.push_back(text_leaf(3, "hi", false, "120px", "24px"));
    const LayoutInput scene = root({std::move(container)});
    const LayoutResult result = engine.layout(scene, LayoutViewport{400.0f, 400.0f});
    const LayoutBox* box = find(result, 3);
    return expect(box != nullptr && box->text_fit == TextFit::Unknown,
                  "a text leaf under display: none stays unknown");
}

bool test_a_text_leaf_with_a_tight_line_height_is_unknown() {
    const LayoutEngine engine;
    // An explicit 8 px `line-height` puts 19.2 px of glyph ink into an 8 px line box, which the
    // wrap reports as 8 px tall. The measure would understate the emission, so the fact must not be
    // claimed even though the box nominally fits.
    LayoutInput leaf = text_leaf(2, "hi", false, "120px", "24px");
    leaf.style.set("line-height", "8px");
    const LayoutInput scene = root({std::move(leaf)});
    return expect(fit_of(engine, scene, 2) == TextFit::Unknown,
                  "a line-height tighter than the font's line box is not provable");
}

// --- The content box is the padding box --------------------------------------------------------

bool test_the_content_box_subtracts_padding_but_not_border() {
    const LayoutEngine engine;
    // Border box 100 px wide, 10 px of padding and 5 px of border on each side. Paint's content box
    // is 100 - 10 - 10 = 80 px; counting the border too would make it 70 px. The run below is
    // exactly 80 px wide, so `Fits` proves the padding-only box: with the border counted it would
    // overflow.
    LayoutInput leaf = text_leaf(2, "abcdefghij", false, "100px", "40px");
    leaf.style.set("padding-left", "10px");
    leaf.style.set("padding-right", "10px");
    leaf.style.set("border-left-width", "5px");
    leaf.style.set("border-left-style", "solid");
    leaf.style.set("border-right-width", "5px");
    leaf.style.set("border-right-style", "solid");
    const LayoutInput scene = root({std::move(leaf)});
    return expect(fit_of(engine, scene, 2) == TextFit::Fits,
                  "the content box subtracts padding only, not the border");
}

bool test_padding_alone_can_make_the_text_overflow() {
    const LayoutEngine engine;
    // The same 80 px run in a 100 px box with 20 px of padding per side: the content box is 60 px,
    // so the text overflows it even though it fits the border box.
    LayoutInput leaf = text_leaf(2, "abcdefghij", false, "100px", "40px");
    leaf.style.set("padding-left", "20px");
    leaf.style.set("padding-right", "20px");
    const LayoutInput scene = root({std::move(leaf)});
    return expect(fit_of(engine, scene, 2) == TextFit::Overflows,
                  "padding narrows the content box the text must stay inside");
}

// --- apply_layout copies the fact onto the node ------------------------------------------------

bool test_apply_layout_copies_and_clears_the_hint() {
    Node root_node(1);
    Node& fit_node = root_node.add_child(std::make_unique<Node>(2));
    Node& missing = root_node.add_child(std::make_unique<Node>(3));
    Node& overflow_node = root_node.add_child(std::make_unique<Node>(4));
    // Pre-set every hint so the test proves `apply_layout` rewrites it rather than merely leaving
    // the default in place.
    fit_node.set_text_fits_box(true);
    missing.set_text_fits_box(true);
    overflow_node.set_text_fits_box(true);

    LayoutResult result;
    result.boxes.push_back(LayoutBox{1, {0.0f, 0.0f, 400.0f, 400.0f}, {}, TextFit::Unknown});
    result.boxes.push_back(LayoutBox{2, {0.0f, 0.0f, 120.0f, 24.0f}, {}, TextFit::Fits});
    // Node 3 deliberately has no box, and node 4's box is measured but does not fit.
    result.boxes.push_back(LayoutBox{4, {0.0f, 24.0f, 40.0f, 20.0f}, {}, TextFit::Overflows});

    const tessera::ui::layout::LayoutApplyResult applied = apply_layout(result, root_node);
    return expect(applied.applied == 3, "three boxes applied") &&
           expect(applied.missing == 1, "the boxless node is reported missing") &&
           expect(fit_node.text_fits_box(), "a Fits box sets the node hint") &&
           expect(!overflow_node.text_fits_box(), "an Overflows box clears the node hint") &&
           expect(!missing.text_fits_box(), "a missing box clears the node hint");
}

} // namespace

} // namespace layout_text_fit_test

int main() {
    bool ok = true;
    ok = layout_text_fit_test::test_text_that_fits_is_proved() && ok;
    ok = layout_text_fit_test::test_wrapped_text_that_still_fits_is_proved() && ok;
    ok = layout_text_fit_test::test_text_taller_than_the_content_box_overflows() && ok;
    ok = layout_text_fit_test::test_text_wider_than_the_content_box_overflows() && ok;
    ok = layout_text_fit_test::test_the_wrap_opt_in_decides_the_result() && ok;
    ok = layout_text_fit_test::test_a_non_wrapping_white_space_suppresses_the_wrap() && ok;
    ok = layout_text_fit_test::test_a_node_without_text_is_unknown() && ok;
    ok = layout_text_fit_test::test_a_display_none_subtree_is_unknown() && ok;
    ok = layout_text_fit_test::test_a_text_leaf_with_a_tight_line_height_is_unknown() && ok;
    ok = layout_text_fit_test::test_the_content_box_subtracts_padding_but_not_border() && ok;
    ok = layout_text_fit_test::test_padding_alone_can_make_the_text_overflow() && ok;
    ok = layout_text_fit_test::test_apply_layout_copies_and_clears_the_hint() && ok;
    if (!ok) {
        std::cerr << "layout_text_fit: failed\n";
        return 1;
    }
    std::cout << "layout_text_fit: ok\n";
    return 0;
}
