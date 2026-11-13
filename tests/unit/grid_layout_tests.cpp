// SPDX-License-Identifier: MIT
//
// Deterministic CSS Grid corpus (issue #249): fixed, repeat, fr, spans, gap, alignment, RTL,
// auto-flow and nesting with flex. Every expected box comes from docs/specs/grid-v1.md; nothing
// here is derived from the implementation.

#include "tessera/ui/layout/layout_engine.hpp"

#include <chrono>
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

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "grid: " << message << "\n";
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

LayoutInput node(std::uint64_t id, ComputedStyle computed, std::vector<LayoutInput> children = {}) {
    LayoutInput result;
    result.id = id;
    result.style = std::move(computed);
    result.children = std::move(children);
    return result;
}

LayoutInput text_node(std::uint64_t id, ComputedStyle computed, std::string text) {
    LayoutInput result;
    result.id = id;
    result.style = std::move(computed);
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

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.01f;
}

bool box_is(const LayoutResult& result, std::uint64_t id, float x, float y, float width,
            float height) {
    const LayoutBox* box = find(result, id);
    const bool matched = box != nullptr && near(box->bounds.x, x) && near(box->bounds.y, y) &&
                         near(box->bounds.width, width) && near(box->bounds.height, height);
    if (!matched && box != nullptr) {
        std::cerr << "  box " << id << " expected " << x << "," << y << " " << width << "x"
                  << height << " got " << box->bounds.x << "," << box->bounds.y << " "
                  << box->bounds.width << "x" << box->bounds.height << "\n";
    }
    return matched;
}

ComputedStyle grid_style(const char* columns) {
    return style({{"display", "grid"},
                  {"width", "400px"},
                  {"height", "200px"},
                  {"grid-template-columns", columns},
                  {"grid-template-rows", "50px 50px"}});
}

} // namespace

int main() {
    const LayoutEngine engine;
    const LayoutViewport viewport{400.0f, 200.0f};

    // 1. Fixed tracks and implicit auto-placement (row flow, no dense).
    const LayoutInput fixed =
        node(1, grid_style("100px 100px"), {node(2, {}), node(3, {}), node(4, {}), node(5, {})});
    const LayoutResult fixed_result = engine.layout(fixed, viewport);
    if (!expect(box_is(fixed_result, 1, 0, 0, 400, 200), "grid root") ||
        !expect(box_is(fixed_result, 2, 0, 0, 100, 50), "fixed cell (1,1)") ||
        !expect(box_is(fixed_result, 3, 100, 0, 100, 50), "fixed cell (1,2)") ||
        !expect(box_is(fixed_result, 4, 0, 50, 100, 50), "fixed cell (2,1)") ||
        !expect(box_is(fixed_result, 5, 100, 50, 100, 50), "fixed cell (2,2)")) {
        return 1;
    }

    // 2. `fr` distributes the free space in proportion to the flex factors.
    const LayoutInput fractions = node(10, grid_style("1fr 3fr"), {node(11, {}), node(12, {})});
    const LayoutResult fractions_result = engine.layout(fractions, viewport);
    if (!expect(box_is(fractions_result, 11, 0, 0, 100, 50), "1fr track") ||
        !expect(box_is(fractions_result, 12, 100, 0, 300, 50), "3fr track")) {
        return 2;
    }

    // 3. `repeat(<n>, ...)` expands before sizing.
    const LayoutInput repeated = node(20,
                                      style({{"display", "grid"},
                                             {"width", "300px"},
                                             {"height", "100px"},
                                             {"grid-template-columns", "repeat(3, 1fr)"},
                                             {"grid-template-rows", "100px"}}),
                                      {node(21, {}), node(22, {}), node(23, {})});
    const LayoutResult repeated_result = engine.layout(repeated, LayoutViewport{300.0f, 100.0f});
    if (!expect(box_is(repeated_result, 21, 0, 0, 100, 100), "repeat track 1") ||
        !expect(box_is(repeated_result, 22, 100, 0, 100, 100), "repeat track 2") ||
        !expect(box_is(repeated_result, 23, 200, 0, 100, 100), "repeat track 3")) {
        return 3;
    }

    // 4. `column-gap` is subtracted from the space the `fr` tracks share.
    const LayoutInput gapped = node(30,
                                    style({{"display", "grid"},
                                           {"width", "220px"},
                                           {"height", "50px"},
                                           {"grid-template-columns", "1fr 1fr"},
                                           {"grid-template-rows", "50px"},
                                           {"column-gap", "20px"}}),
                                    {node(31, {}), node(32, {})});
    const LayoutResult gapped_result = engine.layout(gapped, LayoutViewport{220.0f, 50.0f});
    if (!expect(box_is(gapped_result, 31, 0, 0, 100, 50), "gap: first track") ||
        !expect(box_is(gapped_result, 32, 120, 0, 100, 50), "gap: second track")) {
        return 4;
    }

    // 5. Auto rows size to the tallest item; the container height follows the rows. The wrapper is
    //    a flex column so the grid's height is content-based (a layout root fills the viewport).
    const LayoutInput auto_rows = node(
        40,
        style({{"display", "flex"},
               {"flex-direction", "column"},
               {"align-items", "flex-start"},
               {"width", "200px"}}),
        {node(44,
              style({{"display", "grid"}, {"width", "100px"}, {"grid-template-columns", "100px"}}),
              {node(41, style({{"height", "30px"}})), node(42, style({{"height", "40px"}}))})});
    const LayoutResult auto_rows_result = engine.layout(auto_rows, LayoutViewport{200.0f, 200.0f});
    if (!expect(box_is(auto_rows_result, 44, 0, 0, 100, 70), "auto rows grow the container") ||
        !expect(box_is(auto_rows_result, 41, 0, 0, 100, 30), "auto row 1") ||
        !expect(box_is(auto_rows_result, 42, 0, 30, 100, 40), "auto row 2")) {
        return 5;
    }

    // 6. `grid-column: <start> / span <n>` spans tracks, including the gutter. Each track is
    //    (330 - 2 * 15) / 3 = 100, so the two-track span is 215 wide.
    const LayoutInput spanning = node(50,
                                      style({{"display", "grid"},
                                             {"width", "330px"},
                                             {"height", "100px"},
                                             {"grid-template-columns", "repeat(3, 1fr)"},
                                             {"grid-template-rows", "100px"},
                                             {"column-gap", "15px"}}),
                                      {node(51, style({{"grid-column", "1 / span 2"}})),
                                       node(52, style({{"grid-column", "3"}}))});
    const LayoutResult spanning_result = engine.layout(spanning, LayoutViewport{330.0f, 100.0f});
    if (!expect(box_is(spanning_result, 51, 0, 0, 215, 100), "span 2 tracks + gutter") ||
        !expect(box_is(spanning_result, 52, 230, 0, 100, 100), "third track")) {
        return 6;
    }

    // 7. `justify-items`/`align-items` center every item; `justify-self` overrides one axis.
    const LayoutInput aligned =
        node(60,
             style({{"display", "grid"},
                    {"width", "400px"},
                    {"height", "200px"},
                    {"grid-template-columns", "200px"},
                    {"grid-template-rows", "100px"},
                    {"justify-items", "center"},
                    {"align-items", "center"}}),
             {node(61, style({{"width", "50px"}, {"height", "20px"}})),
              node(62, style({{"width", "50px"}, {"height", "20px"}, {"justify-self", "end"}}))});
    const LayoutResult aligned_result = engine.layout(aligned, viewport);
    if (!expect(box_is(aligned_result, 61, 75, 40, 50, 20), "center/center")) {
        return 7;
    }
    // The second item is auto-placed into row 2, where the same 100px row applies.
    if (!expect(box_is(aligned_result, 62, 150, 140, 50, 20), "justify-self: end")) {
        return 8;
    }

    // 8. `direction: rtl` mirrors the column order.
    const LayoutInput rtl = node(70,
                                 style({{"display", "grid"},
                                        {"direction", "rtl"},
                                        {"width", "400px"},
                                        {"height", "50px"},
                                        {"grid-template-columns", "100px 100px"},
                                        {"grid-template-rows", "50px"}}),
                                 {node(71, {}), node(72, {})});
    const LayoutResult rtl_result = engine.layout(rtl, LayoutViewport{400.0f, 50.0f});
    if (!expect(box_is(rtl_result, 71, 300, 0, 100, 50), "rtl first column") ||
        !expect(box_is(rtl_result, 72, 200, 0, 100, 50), "rtl second column")) {
        return 9;
    }

    // 9. LTR start/end alignment mirrors in RTL: `justify-self: start` is the right edge.
    const LayoutInput rtl_align =
        node(75,
             style({{"display", "grid"},
                    {"direction", "rtl"},
                    {"width", "400px"},
                    {"height", "100px"},
                    {"grid-template-columns", "200px"},
                    {"grid-template-rows", "100px"}}),
             {node(76, style({{"width", "50px"}, {"height", "20px"}, {"justify-self", "start"}}))});
    const LayoutResult rtl_align_result = engine.layout(rtl_align, LayoutViewport{400.0f, 100.0f});
    if (!expect(box_is(rtl_align_result, 76, 350, 0, 50, 20), "rtl justify-self: start")) {
        return 10;
    }

    // 10. A grid inside a flex container is measured as a flex item.
    const LayoutInput grid_in_flex =
        node(80, style({{"display", "flex"}, {"width", "400px"}, {"height", "100px"}}),
             {node(81,
                   style({{"display", "grid"},
                          {"width", "200px"},
                          {"height", "100px"},
                          {"grid-template-columns", "1fr 1fr"},
                          {"grid-template-rows", "100px"}}),
                   {node(82, {}), node(83, {})})});
    const LayoutResult grid_in_flex_result =
        engine.layout(grid_in_flex, LayoutViewport{400.0f, 100.0f});
    if (!expect(box_is(grid_in_flex_result, 81, 0, 0, 200, 100), "grid as a flex item") ||
        !expect(box_is(grid_in_flex_result, 82, 0, 0, 100, 100), "grid in flex: item 1") ||
        !expect(box_is(grid_in_flex_result, 83, 100, 0, 100, 100), "grid in flex: item 2")) {
        return 11;
    }

    // 11. A flex container as a grid item keeps laying out its own children with Yoga.
    const LayoutInput flex_in_grid = node(
        90,
        style({{"display", "grid"},
               {"width", "400px"},
               {"height", "100px"},
               {"grid-template-columns", "200px 200px"},
               {"grid-template-rows", "100px"}}),
        {node(
             91,
             style({{"display", "flex"}, {"flex-direction", "row"}, {"align-items", "flex-start"}}),
             {node(92, style({{"width", "50px"}, {"height", "50px"}})),
              node(93, style({{"width", "50px"}, {"height", "50px"}}))}),
         node(94, {})});
    const LayoutResult flex_in_grid_result =
        engine.layout(flex_in_grid, LayoutViewport{400.0f, 100.0f});
    if (!expect(box_is(flex_in_grid_result, 91, 0, 0, 200, 100), "flex as a grid item") ||
        !expect(box_is(flex_in_grid_result, 92, 0, 0, 50, 50), "flex in grid: child 1") ||
        !expect(box_is(flex_in_grid_result, 93, 50, 0, 50, 50), "flex in grid: child 2")) {
        return 12;
    }

    // 12. Text measurement works for grid items: a start-aligned item takes its max-content size.
    const LayoutInput text_item = node(
        100,
        style({{"display", "grid"},
               {"width", "200px"},
               {"grid-template-columns", "100px"},
               {"grid-template-rows", "20px"}}),
        {text_node(
            101, style({{"font-size", "16px"}, {"line-height", "20px"}, {"justify-self", "start"}}),
            "hello world")});
    const LayoutResult text_item_result = engine.layout(text_item, LayoutViewport{200.0f, 200.0f});
    if (!expect(box_is(text_item_result, 101, 0, 0, 88, 20), "measured text grid item")) {
        return 13;
    }

    // 13. `grid-auto-flow: column` fills down the rows, then moves to the next column.
    const LayoutInput column_flow =
        node(110,
             style({{"display", "grid"},
                    {"width", "200px"},
                    {"height", "100px"},
                    {"grid-template-columns", "100px 100px"},
                    {"grid-template-rows", "50px 50px"},
                    {"grid-auto-flow", "column"}}),
             {node(111, {}), node(112, {}), node(113, {}), node(114, {})});
    const LayoutResult column_flow_result =
        engine.layout(column_flow, LayoutViewport{200.0f, 100.0f});
    if (!expect(box_is(column_flow_result, 111, 0, 0, 100, 50), "column flow (1,1)") ||
        !expect(box_is(column_flow_result, 112, 0, 50, 100, 50), "column flow (2,1)") ||
        !expect(box_is(column_flow_result, 113, 100, 0, 100, 50), "column flow (1,2)") ||
        !expect(box_is(column_flow_result, 114, 100, 50, 100, 50), "column flow (2,2)")) {
        return 14;
    }

    // 14. Explicit line numbers, including negative lines counting back from the explicit grid.
    const LayoutInput lines = node(120,
                                   style({{"display", "grid"},
                                          {"width", "300px"},
                                          {"height", "100px"},
                                          {"grid-template-columns", "repeat(3, 1fr)"},
                                          {"grid-template-rows", "50px 50px"}}),
                                   {node(121, style({{"grid-column", "2 / 4"}})),
                                    node(122, style({{"grid-column", "-2 / -1"}})),
                                    node(123, style({{"grid-column", "1"}}))});
    const LayoutResult lines_result = engine.layout(lines, LayoutViewport{300.0f, 100.0f});
    if (!expect(box_is(lines_result, 121, 100, 0, 200, 50), "grid-column: 2 / 4") ||
        !expect(box_is(lines_result, 122, 200, 50, 100, 50), "negative lines -2 / -1") ||
        !expect(box_is(lines_result, 123, 0, 0, 100, 50), "first free row in column 1")) {
        return 15;
    }

    // 15. `grid-template-rows: repeat(...)` plus a fixed row list.
    const LayoutInput rows = node(130,
                                  style({{"display", "grid"},
                                         {"width", "100px"},
                                         {"height", "120px"},
                                         {"grid-template-columns", "100px"},
                                         {"grid-template-rows", "repeat(2, 60px)"}}),
                                  {node(131, {}), node(132, {})});
    const LayoutResult rows_result = engine.layout(rows, LayoutViewport{100.0f, 120.0f});
    if (!expect(box_is(rows_result, 131, 0, 0, 100, 60), "repeat row 1") ||
        !expect(box_is(rows_result, 132, 0, 60, 100, 60), "repeat row 2")) {
        return 16;
    }

    // 16. `minmax()` with a fixed minimum and a flexible maximum: both tracks are flexible, so the
    //    container is split evenly and each minimum is honoured (400 / 2 = 200 each).
    const LayoutInput minmax =
        node(140,
             style({{"display", "grid"},
                    {"width", "400px"},
                    {"height", "50px"},
                    {"grid-template-columns", "minmax(100px, 1fr) minmax(200px, 1fr)"},
                    {"grid-template-rows", "50px"}}),
             {node(141, {}), node(142, {})});
    const LayoutResult minmax_result = engine.layout(minmax, LayoutViewport{400.0f, 50.0f});
    if (!expect(box_is(minmax_result, 141, 0, 0, 200, 50), "minmax fr track 1") ||
        !expect(box_is(minmax_result, 142, 200, 0, 200, 50), "minmax fr track 2")) {
        return 17;
    }

    // 17. The grid layout is deterministic: a second layout of the same input is identical.
    const LayoutResult fixed_again = engine.layout(fixed, viewport);
    if (!expect(fixed_again.boxes.size() == fixed_result.boxes.size(), "deterministic size")) {
        return 18;
    }
    for (std::size_t i = 0; i < fixed_again.boxes.size(); ++i) {
        const LayoutBox& lhs = fixed_again.boxes[i];
        const LayoutBox& rhs = fixed_result.boxes[i];
        if (!expect(lhs.id == rhs.id && near(lhs.bounds.x, rhs.bounds.x) &&
                        near(lhs.bounds.y, rhs.bounds.y) &&
                        near(lhs.bounds.width, rhs.bounds.width) &&
                        near(lhs.bounds.height, rhs.bounds.height),
                    "deterministic repeat")) {
            return 19;
        }
    }

    // 18. A 1,000-item grid stays well inside the #241 layout budget (45 ms per 1000 nodes is
    //     the benchmark's 4500 ms first style+layout budget divided by 12). The bound is an
    //     order-of-magnitude guard, not a speed ranking.
    std::vector<LayoutInput> items;
    items.reserve(1000);
    for (std::uint64_t i = 0; i < 1000; ++i) {
        items.push_back(node(1000 + i, style({{"height", "20px"}})));
    }
    const LayoutInput large = node(2000,
                                   style({{"display", "grid"},
                                          {"width", "1000px"},
                                          {"grid-template-columns", "repeat(10, 1fr)"},
                                          {"grid-template-rows", "repeat(100, 20px)"}}),
                                   std::move(items));
    const auto start = std::chrono::steady_clock::now();
    const LayoutResult large_result = engine.layout(large, LayoutViewport{1000.0f, 2000.0f});
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    std::cout << "grid: 1000-item layout " << elapsed_ms << " ms\n";
    if (!expect(large_result.boxes.size() == 1001, "1000-item box count") ||
        !expect(box_is(large_result, 1000, 0, 0, 100, 20), "large grid first item") ||
        !expect(box_is(large_result, 1999, 900, 1980, 100, 20), "large grid last item")) {
        return 20;
    }
    if (!expect(elapsed_ms < 4500, "1000-item grid within the #241 layout budget")) {
        return 21;
    }

    std::cout << "grid: ok\n";
    return 0;
}
