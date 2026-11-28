// SPDX-License-Identifier: MIT
//
// Intrinsic sizing found by the page-parity runs of the Studio samples (issue #1018), each checked
// against what a browser does for the same CSS: a container item's min-content is its longest
// unbreakable run, a grid row is as tall as its items are at their column width, a size-contained
// grid item still fills its area, and a control's inline-block shrink applies to a block parent
// only (a flex item of a flex container stretches).

#include "tessera/ui/layout/layout_engine.hpp"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::ui::layout::LayoutBox;
using tessera::ui::layout::LayoutEngine;
using tessera::ui::layout::LayoutInput;
using tessera::ui::layout::LayoutResult;
using tessera::ui::layout::LayoutViewport;
using tessera::ui::style::ComputedStyle;

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layout_intrinsic: " << message << "\n";
        ++failures;
    }
}

ComputedStyle style(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle result;
    for (const auto& [property, value] : values) {
        result.set(property, value);
    }
    return result;
}

LayoutInput node(std::uint64_t id, ComputedStyle computed, std::vector<LayoutInput> children = {}) {
    LayoutInput result;
    result.id = id;
    result.style = std::move(computed);
    result.children = std::move(children);
    return result;
}

// The stub measurer is 8px per character at 16px, with the line height written here.
LayoutInput text_node(std::uint64_t id, std::string text, ComputedStyle extra = {}) {
    LayoutInput result;
    result.id = id;
    result.style = std::move(extra);
    result.style.set("font-size", "16px");
    result.style.set("line-height", "20px");
    result.text = std::move(text);
    return result;
}

const LayoutBox* find(const LayoutResult& result, std::uint64_t id) {
    for (const LayoutBox& box : result.boxes) {
        if (box.id == id) {
            return &box;
        }
    }
    return nullptr;
}

bool box_is(const LayoutResult& result, std::uint64_t id, float x, float y, float width,
            float height) {
    const LayoutBox* box = find(result, id);
    const auto near = [](float lhs, float rhs) { return std::abs(lhs - rhs) <= 0.01f; };
    const bool matched = box != nullptr && near(box->bounds.x, x) && near(box->bounds.y, y) &&
                         near(box->bounds.width, width) && near(box->bounds.height, height);
    if (!matched && box != nullptr) {
        std::cerr << "  box " << id << " expected " << x << "," << y << " " << width << "x"
                  << height << " got " << box->bounds.x << "," << box->bounds.y << " "
                  << box->bounds.width << "x" << box->bounds.height << "\n";
    }
    return matched;
}

void container_min_content_is_the_longest_word() {
    // Two 1fr columns in 100px; each item is a box holding "hello world" (88px on one line, 40px
    // for the longest word). Min-content is 40, so the columns are 50/50 and the text wraps. A
    // container that reported its max-content (88) as its minimum pushed the second column to 88.
    const LayoutEngine engine;
    const LayoutInput grid = node(
        1,
        style({{"display", "grid"},
               {"width", "100px"},
               {"align-content", "start"},
               {"grid-template-columns", "1fr 1fr"}}),
        {node(2, {}, {text_node(3, "hello world")}), node(4, {}, {text_node(5, "hello world")})});
    const LayoutResult result = engine.layout(grid, LayoutViewport{100.0f, 100.0f});
    expect(box_is(result, 2, 0, 0, 50, 40), "first card is half the grid and two lines tall");
    expect(box_is(result, 4, 50, 0, 50, 40), "second card starts at the half");
}

void grid_row_is_as_tall_as_its_items_at_their_column_width() {
    // The first item is one line at max-content width but two at its 50px column; the implicit
    // auto row must be sized for two, so the item in the next row starts at 40.
    const LayoutEngine engine;
    const LayoutInput grid =
        node(10,
             style({{"display", "grid"},
                    {"width", "100px"},
                    {"align-content", "start"},
                    {"grid-template-columns", "50px 50px"}}),
             {text_node(11, "hello world"), text_node(12, "ab"), text_node(13, "cd")});
    const LayoutResult result = engine.layout(grid, LayoutViewport{100.0f, 100.0f});
    expect(box_is(result, 11, 0, 0, 50, 40), "wrapped item fills its two-line row");
    expect(box_is(result, 13, 0, 40, 50, 20), "the next row starts below the wrapped one");
}

void size_contained_grid_item_fills_its_area() {
    // `container-type: inline-size` contributes zero to track sizing, but a stretched item still
    // fills the track it is placed in.
    const LayoutEngine engine;
    const LayoutInput grid = node(
        20,
        style({{"display", "grid"},
               {"width", "300px"},
               {"align-content", "start"},
               {"grid-template-columns", "1fr"}}),
        {node(21, style({{"container-type", "inline-size"}}), {text_node(22, "hello world")})});
    const LayoutResult result = engine.layout(grid, LayoutViewport{300.0f, 100.0f});
    expect(box_is(result, 21, 0, 0, 300, 20), "the contained item is as wide as the grid");
}

void size_contained_block_child_fills_its_parent() {
    const LayoutEngine engine;
    const LayoutInput block = node(
        50, style({{"display", "block"}, {"width", "300px"}}),
        {node(51, style({{"container-type", "inline-size"}, {"height", "10px"}})),
         node(52,
              style({{"container-type", "inline-size"}, {"width", "40px"}, {"height", "10px"}}))});
    const LayoutResult result = engine.layout(block, LayoutViewport{300.0f, 100.0f});
    expect(box_is(result, 51, 0, 0, 300, 10), "a contained block child fills its parent");
    expect(box_is(result, 52, 0, 10, 40, 10), "an authored width on a container still wins");
}

void control_stretches_in_a_flex_column_and_shrinks_in_a_block() {
    // The user-agent sheet's control marker: a flex item stretches, an inline-block in a block
    // parent takes its own width.
    const LayoutEngine engine;
    const LayoutInput flex =
        node(30, style({{"display", "flex"}, {"flex-direction", "column"}, {"width", "200px"}}),
             {text_node(31, "Add", style({{"align-self", "-tessera-inline"}}))});
    const LayoutResult flexed = engine.layout(flex, LayoutViewport{200.0f, 100.0f});
    expect(box_is(flexed, 31, 0, 0, 200, 20), "a control in a flex column stretches");

    const LayoutInput block =
        node(40, style({{"display", "block"}, {"width", "200px"}}),
             {text_node(41, "Add", style({{"align-self", "-tessera-inline"}}))});
    const LayoutResult blocked = engine.layout(block, LayoutViewport{200.0f, 100.0f});
    expect(box_is(blocked, 41, 0, 0, 24, 20), "a control in a block parent takes its own width");
}

} // namespace

int main() {
    container_min_content_is_the_longest_word();
    grid_row_is_as_tall_as_its_items_at_their_column_width();
    size_contained_grid_item_fills_its_area();
    size_contained_block_child_fills_its_parent();
    control_stretches_in_a_flex_column_and_shrinks_in_a_block();
    if (failures == 0) {
        std::cout << "layout_intrinsic: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
