// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/layout_engine.hpp"

#include <cmath>
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
        std::cerr << "layout: " << message << "\n";
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
    return LayoutInput{id, std::move(computed), std::move(children), {}};
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

} // namespace

int main() {
    const LayoutEngine engine;
    const LayoutViewport viewport{400.0f, 200.0f};

    const LayoutInput row = node(1,
                                 style({{"display", "flex"},
                                        {"width", "400px"},
                                        {"height", "200px"},
                                        {"align-items", "flex-start"}}),
                                 {node(2, style({{"width", "100px"}, {"height", "50px"}})),
                                  node(3, style({{"flex-grow", "1"}, {"height", "50px"}}))});
    const LayoutResult row_result = engine.layout(row, viewport);
    if (!expect(box_is(row_result, 1, 0, 0, 400, 200), "root box") ||
        !expect(box_is(row_result, 2, 0, 0, 100, 50), "fixed child") ||
        !expect(box_is(row_result, 3, 100, 0, 300, 50), "grow child")) {
        return 1;
    }

    const LayoutInput column = node(10,
                                    style({{"display", "flex"},
                                           {"flex-direction", "column"},
                                           {"justify-content", "space-between"},
                                           {"align-items", "flex-start"},
                                           {"width", "200px"},
                                           {"height", "200px"}}),
                                    {node(11, style({{"width", "20px"}, {"height", "20px"}})),
                                     node(12, style({{"width", "20px"}, {"height", "30px"}}))});
    const LayoutResult column_result = engine.layout(column, LayoutViewport{200.0f, 200.0f});
    if (!expect(box_is(column_result, 11, 0, 0, 20, 20), "column first") ||
        !expect(box_is(column_result, 12, 0, 170, 20, 30), "column space-between")) {
        return 2;
    }

    const LayoutInput padded = node(20,
                                    style({{"display", "flex"},
                                           {"box-sizing", "border-box"},
                                           {"width", "400px"},
                                           {"height", "100px"},
                                           {"padding-top", "10px"},
                                           {"padding-right", "10px"},
                                           {"padding-bottom", "10px"},
                                           {"padding-left", "10px"},
                                           {"column-gap", "10px"}}),
                                    {node(21, style({{"width", "50px"}, {"height", "20px"}})),
                                     node(22, style({{"width", "50px"}, {"height", "20px"}}))});
    const LayoutResult padded_result = engine.layout(padded, LayoutViewport{400.0f, 100.0f});
    if (!expect(box_is(padded_result, 21, 10, 10, 50, 20), "padded first") ||
        !expect(box_is(padded_result, 22, 70, 10, 50, 20), "padding + gap")) {
        return 3;
    }

    const LayoutInput absolute = node(30,
                                      style({{"display", "flex"},
                                             {"position", "relative"},
                                             {"width", "400px"},
                                             {"height", "200px"}}),
                                      {node(31, style({{"position", "absolute"},
                                                       {"left", "5px"},
                                                       {"top", "7px"},
                                                       {"width", "30px"},
                                                       {"height", "40px"}}))});
    const LayoutResult absolute_result = engine.layout(absolute, LayoutViewport{400.0f, 200.0f});
    if (!expect(box_is(absolute_result, 31, 5, 7, 30, 40), "absolute inset")) {
        return 4;
    }

    const LayoutInput constrained =
        node(40, style({{"display", "flex"}, {"width", "400px"}, {"height", "200px"}}),
             {node(41, style({{"width", "50%"}, {"max-width", "100px"}, {"height", "20px"}}))});
    const LayoutResult constrained_result =
        engine.layout(constrained, LayoutViewport{400.0f, 200.0f});
    if (!expect(box_is(constrained_result, 41, 0, 0, 100, 20), "percent + max-width")) {
        return 5;
    }

    const LayoutInput ratio =
        node(50, style({{"display", "flex"}, {"width", "300px"}, {"height", "200px"}}),
             {node(51, style({{"width", "100px"}, {"aspect-ratio", "2 / 1"}}))});
    const LayoutResult ratio_result = engine.layout(ratio, LayoutViewport{300.0f, 200.0f});
    if (!expect(box_is(ratio_result, 51, 0, 0, 100, 50), "aspect ratio height")) {
        return 6;
    }

    // An explicit height wins over the ratio, as in CSS (issue #215): the ratio only fills in a
    // dimension that is `auto`.
    const LayoutInput ratio_height = node(
        52, style({{"display", "flex"}, {"width", "300px"}, {"height", "200px"}}),
        {node(53, style({{"width", "100px"}, {"height", "20px"}, {"aspect-ratio", "2 / 1"}}))});
    const LayoutResult ratio_height_result =
        engine.layout(ratio_height, LayoutViewport{300.0f, 200.0f});
    if (!expect(box_is(ratio_height_result, 53, 0, 0, 100, 20), "height wins over aspect ratio")) {
        return 41;
    }

    const LayoutInput hidden =
        node(60, style({{"display", "flex"}, {"width", "200px"}, {"height", "100px"}}),
             {node(61, style({{"display", "none"}, {"width", "100px"}, {"height", "50px"}}))});
    const LayoutResult hidden_result = engine.layout(hidden, LayoutViewport{200.0f, 100.0f});
    if (!expect(box_is(hidden_result, 61, 0, 0, 0, 0), "display none")) {
        return 7;
    }

    const LayoutInput rtl = node(
        70,
        style({{"display", "flex"}, {"direction", "rtl"}, {"width", "300px"}, {"height", "100px"}}),
        {node(71, style({{"width", "50px"}, {"height", "20px"}})),
         node(72, style({{"width", "50px"}, {"height", "20px"}}))});
    const LayoutResult rtl_result = engine.layout(rtl, LayoutViewport{300.0f, 100.0f});
    if (!expect(box_is(rtl_result, 71, 250, 0, 50, 20), "rtl first") ||
        !expect(box_is(rtl_result, 72, 200, 0, 50, 20), "rtl second")) {
        return 8;
    }

    const LayoutInput wrap = node(80,
                                  style({{"display", "flex"},
                                         {"flex-wrap", "wrap"},
                                         {"align-content", "flex-start"},
                                         {"width", "100px"},
                                         {"height", "200px"}}),
                                  {node(81, style({{"width", "60px"}, {"height", "10px"}})),
                                   node(82, style({{"width", "60px"}, {"height", "10px"}}))});
    const LayoutResult wrap_result = engine.layout(wrap, LayoutViewport{100.0f, 200.0f});
    if (!expect(box_is(wrap_result, 81, 0, 0, 60, 10), "wrap first") ||
        !expect(box_is(wrap_result, 82, 0, 10, 60, 10), "wrap second")) {
        return 9;
    }

    const LayoutInput text_wrap = node(
        100,
        style({{"display", "flex"},
               {"flex-direction", "column"},
               {"align-items", "flex-start"},
               {"width", "50px"},
               {"height", "200px"}}),
        {text_node(101, style({{"font-size", "16px"}, {"line-height", "20px"}}), "hello world")});
    const LayoutResult text_wrap_result = engine.layout(text_wrap, LayoutViewport{50.0f, 200.0f});
    if (!expect(box_is(text_wrap_result, 101, 0, 0, 40, 40), "text wraps")) {
        return 11;
    }

    const LayoutInput text_break =
        node(110,
             style({{"display", "flex"},
                    {"flex-direction", "column"},
                    {"align-items", "flex-start"},
                    {"width", "20px"},
                    {"height", "200px"}}),
             {text_node(111, style({{"font-size", "16px"}, {"line-height", "20px"}}), "abcdefgh")});
    const LayoutResult text_break_result = engine.layout(text_break, LayoutViewport{20.0f, 200.0f});
    if (!expect(box_is(text_break_result, 111, 0, 0, 16, 80), "text breaks long word")) {
        return 12;
    }

    const LayoutInput text_nowrap = node(
        120,
        style({{"display", "flex"},
               {"flex-direction", "column"},
               {"align-items", "flex-start"},
               {"width", "30px"},
               {"height", "200px"}}),
        {text_node(
            121, style({{"font-size", "16px"}, {"line-height", "20px"}, {"white-space", "nowrap"}}),
            "hello")});
    const LayoutResult text_nowrap_result =
        engine.layout(text_nowrap, LayoutViewport{30.0f, 200.0f});
    if (!expect(box_is(text_nowrap_result, 121, 0, 0, 40, 20), "text nowrap overflows")) {
        return 13;
    }

    const LayoutInput content_box = node(85, style({{"display", "flex"},
                                                    {"box-sizing", "content-box"},
                                                    {"width", "100px"},
                                                    {"height", "100px"},
                                                    {"padding-top", "10px"},
                                                    {"padding-right", "10px"},
                                                    {"padding-bottom", "10px"},
                                                    {"padding-left", "10px"}}));
    const LayoutResult content_box_result =
        engine.layout(content_box, LayoutViewport{200.0f, 200.0f});
    if (!expect(box_is(content_box_result, 85, 0, 0, 120, 120), "content-box adds padding")) {
        return 10;
    }

    const LayoutInput bordered = node(90, style({{"display", "flex"},
                                                 {"box-sizing", "border-box"},
                                                 {"width", "100px"},
                                                 {"height", "100px"},
                                                 {"padding-top", "10px"},
                                                 {"padding-right", "10px"},
                                                 {"padding-bottom", "10px"},
                                                 {"padding-left", "10px"},
                                                 {"border-top-width", "2px"},
                                                 {"border-right-width", "2px"},
                                                 {"border-bottom-width", "2px"},
                                                 {"border-left-width", "2px"},
                                                 {"border-top-style", "solid"},
                                                 {"border-right-style", "solid"},
                                                 {"border-bottom-style", "solid"},
                                                 {"border-left-style", "solid"}}));
    const LayoutResult bordered_result = engine.layout(bordered, LayoutViewport{100.0f, 100.0f});
    if (!expect(box_is(bordered_result, 90, 0, 0, 100, 100), "border-box dimensions")) {
        return 10;
    }

    const LayoutResult repeat = engine.layout(row, viewport);
    if (!expect(repeat.boxes.size() == row_result.boxes.size(), "repeat size")) {
        return 11;
    }
    for (std::size_t i = 0; i < repeat.boxes.size(); ++i) {
        const LayoutBox& lhs = repeat.boxes[i];
        const LayoutBox& rhs = row_result.boxes[i];
        if (!expect(lhs.id == rhs.id && near(lhs.bounds.x, rhs.bounds.x) &&
                        near(lhs.bounds.y, rhs.bounds.y) &&
                        near(lhs.bounds.width, rhs.bounds.width) &&
                        near(lhs.bounds.height, rhs.bounds.height),
                    "deterministic repeat")) {
            return 12;
        }
    }

    // field-sizing: content (ADR-041 slice 7): height follows the wrapped text, clamped by
    // min/max-height; an empty field is one line.
    const auto field = [&](const char* sizing, const std::string& text, const char* max_height) {
        return engine.layout(
            node(300, style({{"display", "flex"}, {"flex-direction", "column"}, {"width", "50px"}}),
                 {text_node(301,
                            style({{"font-size", "16px"},
                                   {"line-height", "20px"},
                                   {"field-sizing", sizing},
                                   {"min-height", "30px"},
                                   {"max-height", max_height}}),
                            text)}),
            LayoutViewport{200.0f, 600.0f});
    };
    const LayoutResult one_line = field("content", "hi", "100px");
    const LayoutResult wrapped = field("content", "hello world", "100px");
    const LayoutResult clamped = field("content", "hello world hello world", "50px");
    const LayoutResult empty_field = field("content", "", "100px");
    const LayoutBox* one = find(one_line, 301);
    const LayoutBox* two = find(wrapped, 301);
    const LayoutBox* many = find(clamped, 301);
    const LayoutBox* none = find(empty_field, 301);
    if (!expect(one && near(one->bounds.height, 30.0f), "one line grows to min-height") ||
        !expect(two && near(two->bounds.height, 40.0f), "wrapped text grows the field") ||
        !expect(many && near(many->bounds.height, 50.0f), "max-height clamps the field") ||
        !expect(none && near(none->bounds.height, 30.0f), "empty field is one line")) {
        return 13;
    }

    // A percentage width resolves against the containing block's CONTENT box, not its padding box
    // (issue #791 part 2). The Android shell's card has `padding: 24px` and its children have
    // `width: 100%`; resolving against the padding box makes every child end at the card's border
    // edge, so the right padding is never subtracted and the children overflow. The left padding IS
    // applied, which is why the bug looks almost right and is easy to miss.
    const LayoutInput padded_card =
        node(200,
             style({{"display", "flex"},
                    {"flex-direction", "column"},
                    {"width", "400px"},
                    {"height", "200px"},
                    {"padding-top", "24px"},
                    {"padding-right", "24px"},
                    {"padding-bottom", "24px"},
                    {"padding-left", "24px"}}),
             {node(201, style({{"width", "100%"}, {"height", "48px"}})),
              node(202, style({{"width", "100%"}, {"height", "48px"}}))});
    const LayoutResult padded_card_result = engine.layout(padded_card, viewport);
    // The card is `content-box` (the CSS default), so its authored 400x200 is the CONTENT box and
    // the border box is 448x248. A 100% child therefore resolves to 400 wide starting at x=24 -
    // ending at 424, not at the card's 448 border edge.
    if (!expect(box_is(padded_card_result, 200, 0, 0, 448, 248), "padded card box") ||
        !expect(box_is(padded_card_result, 201, 24, 24, 400, 48),
                "a 100% child fits the content box, not the padding box") ||
        !expect(box_is(padded_card_result, 202, 24, 72, 400, 48),
                "the second 100% child fits too")) {
        return 14;
    }

    // CSS size containment (issue #700). `container-type` makes a container's content-box size
    // independent of its descendants on the contained axis; that independence is what keeps a
    // self-referencing `@container` rule from changing the size that selected it.
    //
    // Every case here has an auto height, so the container fills the 200px viewport - ordinary
    // block layout, unrelated to containment. Only the widths are the subject.
    const LayoutInput contained =
        node(300, style({{"container-type", "inline-size"}}),
             {node(301, style({{"width", "400px"}, {"height", "40px"}}))});
    const LayoutResult contained_result = engine.layout(contained, viewport);
    if (!expect(box_is(contained_result, 300, 0, 0, 0, 200),
                "inline-size containment zeroes an auto width") ||
        !expect(box_is(contained_result, 301, 0, 0, 400, 40),
                "the child keeps its own size, overhanging rather than widening the parent")) {
        return 15;
    }

    // The control, without which a bug that zeroed every `auto` width would pass the case above and
    // break every ordinary page.
    const LayoutInput uncontained =
        node(310, style({}), {node(311, style({{"width", "400px"}, {"height", "40px"}}))});
    const LayoutResult uncontained_result = engine.layout(uncontained, viewport);
    if (!expect(box_is(uncontained_result, 310, 0, 0, 400, 200),
                "without container-type an auto width still sizes to content")) {
        return 16;
    }

    // A length the author wrote is never overridden: containment removes a dependency on content,
    // it does not replace a used value.
    const LayoutInput authored_width =
        node(320, style({{"container-type", "inline-size"}, {"width", "250px"}}),
             {node(321, style({{"width", "400px"}, {"height", "40px"}}))});
    const LayoutResult authored_result = engine.layout(authored_width, viewport);
    if (!expect(box_is(authored_result, 320, 0, 0, 250, 200),
                "an authored width wins over containment")) {
        return 17;
    }

    // `size` contains both axes; `inline-size` must leave the block one alone. Pinning the negative
    // case is what stops the two keywords being treated as synonyms.
    const LayoutInput both_axes =
        node(330, style({{"container-type", "size"}}),
             {node(331, style({{"width", "400px"}, {"height", "120px"}}))});
    const LayoutResult both_result = engine.layout(both_axes, viewport);
    if (!expect(box_is(both_result, 330, 0, 0, 0, 0), "size containment zeroes both axes")) {
        return 18;
    }
    const LayoutInput inline_only =
        node(340, style({{"container-type", "inline-size"}}),
             {node(341, style({{"width", "400px"}, {"height", "120px"}}))});
    const LayoutResult inline_only_result = engine.layout(inline_only, viewport);
    // The height is NOT contained here, so it behaves like any auto-height block box and fills the
    // viewport. This is the assertion that fails if `inline-size` is treated as `size`.
    if (!expect(box_is(inline_only_result, 340, 0, 0, 0, 200),
                "inline-size contains the inline axis only; the height is not zeroed")) {
        return 19;
    }

    std::cout << "layout: ok\n";
    return 0;
}
