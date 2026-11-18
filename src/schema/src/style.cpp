// SPDX-License-Identifier: MIT

// The style property table (docs/specs/jsx-intrinsics-v1/machine-readable.md section 5). The rows
// are the longhands of src/ui/style/property_registry.cpp (the physical table there plus
// `logical_properties.cpp` and `property_registry_extra.cpp`), grouped by that file's sections so
// the two can be read side by side; the kind decides whether a number in an inline `style` object
// becomes "<n>px" (decision 10 of docs/design/jsx-ir-lowering.md).
//
// The row set is not derivable in code: the schema is a leaf library that must not link or include
// the UI module (machine-readable.md section 4, `schema_layering`), so the registry cannot be
// queried from here. The `schema_style_registry` ctest keeps the two tables equal in both
// directions and checks each kind against the registry's grammar; change both tables together.

#include "tables.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace tessera::schema::internal {
namespace {

constexpr StyleProperty kStyleProperties[] = {
    // Box model: display, sizing, spacing, borders, radii.
    {"display", StyleKind::Keyword, ""},
    {"position", StyleKind::Keyword, ""},
    {"box-sizing", StyleKind::Keyword, ""},
    {"width", StyleKind::Length, ""},
    {"height", StyleKind::Length, ""},
    {"aspect-ratio", StyleKind::Number, "a bare number is a unitless ratio"},
    {"min-width", StyleKind::Length, ""},
    {"min-height", StyleKind::Length, ""},
    {"max-width", StyleKind::Length, ""},
    {"max-height", StyleKind::Length, ""},
    {"top", StyleKind::Length, ""},
    {"right", StyleKind::Length, ""},
    {"bottom", StyleKind::Length, ""},
    {"left", StyleKind::Length, ""},
    {"margin-top", StyleKind::Length, ""},
    {"margin-right", StyleKind::Length, ""},
    {"margin-bottom", StyleKind::Length, ""},
    {"margin-left", StyleKind::Length, ""},
    {"padding-top", StyleKind::Length, ""},
    {"padding-right", StyleKind::Length, ""},
    {"padding-bottom", StyleKind::Length, ""},
    {"padding-left", StyleKind::Length, ""},
    {"border-top-width", StyleKind::Length, ""},
    {"border-right-width", StyleKind::Length, ""},
    {"border-bottom-width", StyleKind::Length, ""},
    {"border-left-width", StyleKind::Length, ""},
    {"border-top-style", StyleKind::Keyword, ""},
    {"border-right-style", StyleKind::Keyword, ""},
    {"border-bottom-style", StyleKind::Keyword, ""},
    {"border-left-style", StyleKind::Keyword, ""},
    {"border-top-color", StyleKind::Color, ""},
    {"border-right-color", StyleKind::Color, ""},
    {"border-bottom-color", StyleKind::Color, ""},
    {"border-left-color", StyleKind::Color, ""},
    {"border-top-left-radius", StyleKind::Length, ""},
    {"border-top-right-radius", StyleKind::Length, ""},
    {"border-bottom-right-radius", StyleKind::Length, ""},
    {"border-bottom-left-radius", StyleKind::Length, ""},
    {"field-sizing", StyleKind::Keyword, ""},
    {"overflow-x", StyleKind::Keyword, ""},
    {"overflow-y", StyleKind::Keyword, ""},

    // Paint: colors, the shadow list, the two backdrop names and the root-window material.
    {"color", StyleKind::Color, ""},
    {"background-color", StyleKind::Color, ""},
    {"background-image", StyleKind::Keyword, "none or one linear-gradient()"},
    {"opacity", StyleKind::Number, ""},
    {"box-shadow", StyleKind::List, "comma-separated shadow list, one per layer"},
    {"backdrop-filter", StyleKind::Keyword, ""},
    {"-webkit-backdrop-filter", StyleKind::Keyword, "alias of backdrop-filter"},
    {"window-backdrop", StyleKind::Keyword, ""},
    {"z-index", StyleKind::Number, ""},

    // Typography: the font longhands and the text properties.
    {"font-family", StyleKind::List, "comma-separated family list"},
    {"font-size", StyleKind::Length, ""},
    {"font-style", StyleKind::Keyword, ""},
    {"font-weight", StyleKind::Number, "a number stays unitless"},
    {"line-height", StyleKind::Number, "a bare number is a unitless multiplier"},
    {"letter-spacing", StyleKind::Length, ""},
    {"word-spacing", StyleKind::Length, ""},
    {"text-align", StyleKind::Keyword, ""},
    {"text-transform", StyleKind::Keyword, ""},
    {"text-overflow", StyleKind::Keyword, ""},
    {"white-space", StyleKind::Keyword, ""},

    // Interaction and layout direction.
    {"visibility", StyleKind::Keyword, ""},
    {"cursor", StyleKind::Keyword, ""},
    {"pointer-events", StyleKind::Keyword, ""},
    {"user-select", StyleKind::Keyword, ""},
    {"direction", StyleKind::Keyword, ""},
    {"writing-mode", StyleKind::Keyword, ""},

    // Flex: direction, wrap, grow/shrink/basis, justify/align, gap, order.
    {"flex-direction", StyleKind::Keyword, ""},
    {"flex-wrap", StyleKind::Keyword, ""},
    {"flex-grow", StyleKind::Number, ""},
    {"flex-shrink", StyleKind::Number, ""},
    {"flex-basis", StyleKind::Length, ""},
    {"align-items", StyleKind::Keyword, ""},
    {"align-self", StyleKind::Keyword, ""},
    {"align-content", StyleKind::Keyword, ""},
    {"justify-content", StyleKind::Keyword, ""},
    {"justify-items", StyleKind::Keyword, ""},
    {"row-gap", StyleKind::Length, ""},
    {"column-gap", StyleKind::Length, ""},
    {"order", StyleKind::Number, ""},

    // Grid: the track lists and the item placement of docs/specs/grid-v1.md. A track needs a unit
    // or `fr`, and a line is `auto`, `<n>` or `<start> / <end>`, so none of them is a length.
    {"grid-template-columns", StyleKind::List, "track list; a bare number is not a track"},
    {"grid-template-rows", StyleKind::List, "track list; a bare number is not a track"},
    {"grid-column", StyleKind::Keyword, "`<start> / <end>` line value"},
    {"grid-row", StyleKind::Keyword, "`<start> / <end>` line value"},
    {"grid-auto-flow", StyleKind::Keyword, ""},
    {"justify-self", StyleKind::Keyword, ""},

    // Scroll: the four runtime/style inputs of the scroll controller.
    {"scroll-behavior", StyleKind::Keyword, ""},
    {"overscroll-behavior", StyleKind::Keyword, ""},
    {"overscroll-effect", StyleKind::Keyword, ""},
    {"scrollbar-gutter", StyleKind::Keyword, ""},

    // Responsive: the container query inputs.
    {"container-type", StyleKind::Keyword, ""},
    {"container-name", StyleKind::Keyword, ""},

    // Transforms: the function list and its origin (an origin is one or two lengths).
    {"transform", StyleKind::List, "function list; a bare number is not a transform"},
    {"transform-origin", StyleKind::Length, ""},

    // Animations and transitions: eight and four longhands plus the two names whose grammar lives
    // in src/ui/animation/ (the registry only accepts them so the cascade stores them).
    {"animation-name", StyleKind::Keyword, ""},
    {"animation-duration", StyleKind::Number, "a time; a bare number stays unitless (seconds)"},
    {"animation-timing-function", StyleKind::Keyword, ""},
    {"animation-delay", StyleKind::Number, "a time; a bare number stays unitless (seconds)"},
    {"animation-iteration-count", StyleKind::Number, ""},
    {"animation-direction", StyleKind::Keyword, ""},
    {"animation-fill-mode", StyleKind::Keyword, ""},
    {"animation-play-state", StyleKind::Keyword, ""},
    {"animation", StyleKind::Keyword, "shorthand; the grammar lives in src/ui/animation"},
    {"transition-property", StyleKind::List, "comma-separated property names"},
    {"transition-duration", StyleKind::Number, "a time; a bare number stays unitless (seconds)"},
    {"transition-timing-function", StyleKind::Keyword, ""},
    {"transition-delay", StyleKind::Number, "a time; a bare number stays unitless (seconds)"},
    {"transition", StyleKind::Keyword, "shorthand; the grammar lives in src/ui/animation"},

    // Layout direction: the inline-axis logical longhands of stylesheet-v1 section 11.1.
    {"margin-inline-start", StyleKind::Length, ""},
    {"margin-inline-end", StyleKind::Length, ""},
    {"padding-inline-start", StyleKind::Length, ""},
    {"padding-inline-end", StyleKind::Length, ""},
    {"inset-inline-start", StyleKind::Length, ""},
    {"inset-inline-end", StyleKind::Length, ""},
    {"border-inline-start-width", StyleKind::Length, ""},
    {"border-inline-start-style", StyleKind::Keyword, ""},
    {"border-inline-start-color", StyleKind::Color, ""},
    {"border-inline-end-width", StyleKind::Length, ""},
    {"border-inline-end-style", StyleKind::Keyword, ""},
    {"border-inline-end-color", StyleKind::Color, ""},
};

constexpr bool style_rows_are_valid(std::span<const StyleProperty> list) {
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].name.empty()) {
            return false;
        }
        for (std::size_t j = i + 1; j < list.size(); ++j) {
            if (list[i].name == list[j].name) {
                return false;
            }
        }
    }
    return true;
}

// The registry of src/ui/style/property_registry.cpp (the physical table, `logical_properties.cpp`
// and `property_registry_extra.cpp`) lists 120 longhands; the `schema_style_registry` test in that
// module checks that this table and the registry agree name for name and kind for grammar.
static_assert(std::size(kStyleProperties) == 120, "the style table has one row per longhand");
static_assert(style_rows_are_valid(kStyleProperties), "style rows are unique and named");

} // namespace

std::span<const StyleProperty> style_property_table() {
    return kStyleProperties;
}

} // namespace tessera::schema::internal
