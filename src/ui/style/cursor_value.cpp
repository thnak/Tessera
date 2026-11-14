// SPDX-License-Identifier: MIT
//
// The CSS `cursor` value grammar (issue #191): a bounded keyword table shared by the declaration
// validator, `@supports` and the computed-style -> scene sync. Keeping it one table is what makes
// `@supports (cursor: pointer)` agree with what the validator accepts and what the scene stores.

#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/cursor_value.hpp"

#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <utility>

namespace tessera::ui::style {
namespace {

std::string_view trim(std::string_view text) noexcept {
    const auto first = text.find_first_not_of(" \t\r\n\f");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n\f");
    return text.substr(first, last - first + 1);
}

bool equals_lower(std::string_view text, std::string_view keyword) noexcept {
    if (text.size() != keyword.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) != keyword[i]) {
            return false;
        }
    }
    return true;
}

struct CursorKeyword {
    CursorKind kind;
    std::string_view name;
};

// One row per enumerator, in `CursorKind` declaration order (a static_assert below keeps the two in
// step). `none` is CSS `cursor: none` and maps to the HAL's `Hidden`.
constexpr std::array<CursorKeyword, 36> kCursorKeywords{{
    {CursorKind::Auto, "auto"},
    {CursorKind::Default, "default"},
    {CursorKind::None, "none"},
    {CursorKind::ContextMenu, "context-menu"},
    {CursorKind::Help, "help"},
    {CursorKind::Pointer, "pointer"},
    {CursorKind::Progress, "progress"},
    {CursorKind::Wait, "wait"},
    {CursorKind::Cell, "cell"},
    {CursorKind::Crosshair, "crosshair"},
    {CursorKind::Text, "text"},
    {CursorKind::VerticalText, "vertical-text"},
    {CursorKind::Alias, "alias"},
    {CursorKind::Copy, "copy"},
    {CursorKind::Move, "move"},
    {CursorKind::NoDrop, "no-drop"},
    {CursorKind::NotAllowed, "not-allowed"},
    {CursorKind::Grab, "grab"},
    {CursorKind::Grabbing, "grabbing"},
    {CursorKind::AllScroll, "all-scroll"},
    {CursorKind::ZoomIn, "zoom-in"},
    {CursorKind::ZoomOut, "zoom-out"},
    {CursorKind::ResizeN, "n-resize"},
    {CursorKind::ResizeE, "e-resize"},
    {CursorKind::ResizeS, "s-resize"},
    {CursorKind::ResizeW, "w-resize"},
    {CursorKind::ResizeNe, "ne-resize"},
    {CursorKind::ResizeNw, "nw-resize"},
    {CursorKind::ResizeSe, "se-resize"},
    {CursorKind::ResizeSw, "sw-resize"},
    {CursorKind::ResizeNs, "ns-resize"},
    {CursorKind::ResizeEw, "ew-resize"},
    {CursorKind::ResizeNesw, "nesw-resize"},
    {CursorKind::ResizeNwse, "nwse-resize"},
    {CursorKind::ColResize, "col-resize"},
    {CursorKind::RowResize, "row-resize"},
}};

static_assert(kCursorKeywords.size() == static_cast<std::size_t>(CursorKind::RowResize) + 1,
              "kCursorKeywords must list every CursorKind in declaration order");

// The CSS keyword names of a `CursorKind` that is not in the table would be a programming error,
// and the table is exhaustive by the static_assert above, so this is only the "never nullptr"
// fallback.
constexpr std::string_view kUnknownKeyword = "auto";

// A bare CSS identifier: letters, digits and hyphens, starting with a letter or a hyphen. That is
// what separates a keyword from a `url(...)` cursor list, an `<image>` function or a comma list,
// which V1 does not implement.
bool is_identifier(std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool digit = c >= '0' && c <= '9';
        if (i == 0 ? !(alpha || c == '-') : !(alpha || digit || c == '-')) {
            return false;
        }
    }
    return true;
}

} // namespace

bool is_cursor_keyword(std::string_view value) noexcept {
    return is_identifier(trim(value));
}

bool is_supported_cursor_value(std::string_view value) noexcept {
    const std::string_view normalized = trim(value);
    for (const CursorKeyword& keyword : kCursorKeywords) {
        if (equals_lower(normalized, keyword.name)) {
            return true;
        }
    }
    return false;
}

CursorKind parse_cursor_kind(std::string_view value) noexcept {
    const std::string_view normalized = trim(value);
    for (const CursorKeyword& keyword : kCursorKeywords) {
        if (equals_lower(normalized, keyword.name)) {
            return keyword.kind;
        }
    }
    // The CSS fallback: an unrecognised keyword, a `url()` list and a missing property all resolve
    // to the initial value, so V1 never hands a consumer a shape it did not implement.
    return CursorKind::Auto;
}

std::string_view cursor_kind_name(CursorKind kind) noexcept {
    for (const CursorKeyword& keyword : kCursorKeywords) {
        if (keyword.kind == kind) {
            return keyword.name;
        }
    }
    return kUnknownKeyword;
}

void report_unsupported_cursor_value(std::string_view value, std::size_t offset,
                                     std::vector<StyleDiagnostic>& out) {
    const std::string_view normalized = trim(value);
    // CSS-wide keywords are the cascade's business, and a `var()` reference is judged after
    // substitution: neither is a value-form diagnostic here.
    if (normalized.empty() || equals_lower(normalized, "inherit") ||
        equals_lower(normalized, "initial") || equals_lower(normalized, "unset") ||
        equals_lower(normalized, "revert") || equals_lower(normalized, "revert-layer") ||
        normalized.find("var(") != std::string_view::npos ||
        is_supported_cursor_value(normalized)) {
        return;
    }
    std::string message = "cursor does not support the value `";
    message += normalized;
    message += "` (V1 implements the CSS cursor keywords; `url()` cursor lists are not supported)";
    out.push_back(StyleDiagnostic{StyleDiagnosticKind::UnsupportedValue, offset, "cursor",
                                  std::move(message)});
}

} // namespace tessera::ui::style
