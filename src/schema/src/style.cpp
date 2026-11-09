// SPDX-License-Identifier: MIT

// The style property table (docs/specs/jsx-intrinsics-v1/machine-readable.md section 5). The rows
// are the longhands of src/ui/style/property_registry.cpp; the kind decides whether a number in an
// inline `style` object becomes "<n>px" (decision 10 of docs/design/jsx-ir-lowering.md).

#include "tables.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace tessera::schema::internal {
namespace {

constexpr StyleProperty kStyleProperties[] = {
    {"display", StyleKind::Keyword, ""},
    {"position", StyleKind::Keyword, ""},
    {"box-sizing", StyleKind::Keyword, ""},
    {"width", StyleKind::Length, ""},
    {"height", StyleKind::Length, ""},
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
    {"color", StyleKind::Color, ""},
    {"background-color", StyleKind::Color, ""},
    {"opacity", StyleKind::Number, ""},
    {"font-family", StyleKind::List, "comma-separated family list"},
    {"font-size", StyleKind::Length, ""},
    {"font-style", StyleKind::Keyword, ""},
    {"font-weight", StyleKind::Number, "a number stays unitless"},
    {"line-height", StyleKind::Number, "a bare number is a unitless multiplier"},
    {"letter-spacing", StyleKind::Length, ""},
    {"word-spacing", StyleKind::Length, ""},
    {"text-align", StyleKind::Keyword, ""},
    {"text-transform", StyleKind::Keyword, ""},
    {"white-space", StyleKind::Keyword, ""},
    {"visibility", StyleKind::Keyword, ""},
    {"cursor", StyleKind::Keyword, ""},
    {"direction", StyleKind::Keyword, ""},
    {"writing-mode", StyleKind::Keyword, ""},
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
    {"overflow-x", StyleKind::Keyword, ""},
    {"overflow-y", StyleKind::Keyword, ""},
    {"order", StyleKind::Number, ""},
    {"z-index", StyleKind::Number, ""},
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

// The registry of src/ui/style/property_registry.cpp lists 70 longhands; a unit test in that
// module checks that this table and the registry agree name for name.
static_assert(std::size(kStyleProperties) == 70, "the style table has one row per longhand");
static_assert(style_rows_are_valid(kStyleProperties), "style rows are unique and named");

} // namespace

std::span<const StyleProperty> style_property_table() {
    return kStyleProperties;
}

} // namespace tessera::schema::internal
