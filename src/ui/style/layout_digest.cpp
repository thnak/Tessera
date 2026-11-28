// SPDX-License-Identifier: MIT
//
// The layout half of a computed style, as a digest (issue #735, slice A). It lives in the style
// module because `tessera_layout` depends on `tessera_style`: the layout signature folds this in,
// and the layout-unchanged gate compares it before and after a restyle, but neither may be a
// dependency of the style engine.
//
// The property set is exactly the one `layout_signature` documents - the longhands `apply_style`,
// `child_layout_order` and `text_style_from` read, minus the paint-only `text-overflow`. It is kept
// here, beside the digest, so there is one list rather than two that can drift.

#include "tessera/ui/style/layout_digest.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tessera::ui::style {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

// Sorted by byte value so binary search and a compile-time order check apply.
// `scrollbar-gutter` is layout input (it reserves the overlay scrollbar width); the other scroll
// style properties (`scroll-behavior`, `overscroll-behavior`, `overscroll-effect`) are runtime
// inputs for the scroll controller and are deliberately absent so changing one never reflows.
constexpr std::array<std::string_view, 85> kLayoutProperties = {{
    "align-content",
    "align-items",
    "align-self",
    "aspect-ratio",
    "border-bottom-style",
    "border-bottom-width",
    "border-collapse",
    "border-inline-end-style",
    "border-inline-end-width",
    "border-inline-start-style",
    "border-inline-start-width",
    "border-left-style",
    "border-left-width",
    "border-right-style",
    "border-right-width",
    "border-spacing",
    "border-top-style",
    "border-top-width",
    "box-sizing",
    "caption-side",
    "column-gap",
    "direction",
    "display",
    "field-sizing",
    "flex-basis",
    "flex-direction",
    "flex-grow",
    "flex-shrink",
    "flex-wrap",
    "font-feature-settings",
    "font-size",
    "font-style",
    "font-variant-alternates",
    "font-variant-caps",
    "font-variant-east-asian",
    "font-variant-ligatures",
    "font-variant-numeric",
    "font-variant-position",
    "font-weight",
    "grid-auto-flow",
    "grid-column",
    "grid-row",
    "grid-template-columns",
    "grid-template-rows",
    "height",
    "inset-inline-end",
    "inset-inline-start",
    "justify-content",
    "justify-items",
    "justify-self",
    "left",
    "letter-spacing",
    "line-height",
    "list-style-position",
    "list-style-type",
    "margin-bottom",
    "margin-inline-end",
    "margin-inline-start",
    "margin-left",
    "margin-right",
    "margin-top",
    "max-height",
    "max-width",
    "min-height",
    "min-width",
    "order",
    "overflow-x",
    "overflow-y",
    "padding-bottom",
    "padding-inline-end",
    "padding-inline-start",
    "padding-left",
    "padding-right",
    "padding-top",
    "position",
    "right",
    "row-gap",
    "scrollbar-gutter",
    "table-layout",
    "text-transform",
    "top",
    "vertical-align",
    "white-space",
    "width",
    "word-spacing",
}};

constexpr bool properties_are_sorted() {
    for (std::size_t i = 1; i < kLayoutProperties.size(); ++i) {
        if (!(kLayoutProperties[i - 1] < kLayoutProperties[i])) {
            return false;
        }
    }
    return true;
}

static_assert(properties_are_sorted(), "kLayoutProperties must stay sorted for binary search");

// Framing bytes keep the hash unambiguous: a property index and its value are delimited, so
// shifting bytes between fields changes the digest.
constexpr std::uint8_t kProperty = 0x50;

class Hasher {
public:
    void u8(std::uint8_t value) noexcept {
        state_ = (state_ ^ value) * kFnvPrime;
    }

    void u32(std::uint32_t value) noexcept {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
        }
    }

    void bytes(std::string_view value) noexcept {
        u32(static_cast<std::uint32_t>(value.size()));
        for (const char c : value) {
            u8(static_cast<std::uint8_t>(c));
        }
    }

    [[nodiscard]] std::uint64_t value() const noexcept {
        return state_;
    }

private:
    std::uint64_t state_ = kFnvOffset;
};

// The index of `name` in the layout property set, or kLayoutProperties.size() when the property is
// paint-only/unknown and must not invalidate a layout.
std::size_t layout_property_index(std::string_view name) noexcept {
    const auto found = std::lower_bound(kLayoutProperties.begin(), kLayoutProperties.end(), name);
    if (found == kLayoutProperties.end() || *found != name) {
        return kLayoutProperties.size();
    }
    return static_cast<std::size_t>(found - kLayoutProperties.begin());
}

} // namespace

bool is_layout_property(std::string_view property) noexcept {
    return layout_property_index(property) != kLayoutProperties.size();
}

std::uint64_t layout_digest(const ComputedStyle& style) noexcept {
    const std::uint64_t cached = style.cached_digest();
    if (cached != 0) {
        return cached;
    }
    Hasher hasher;
    // The effective style is sorted by name, so this walk is deterministic regardless of insertion
    // order. Only the layout set is folded in; a paint-only change leaves the digest intact.
    for (const auto& [name, value] : style.properties()) {
        const std::size_t index = layout_property_index(name);
        if (index == kLayoutProperties.size()) {
            continue;
        }
        hasher.u8(kProperty);
        hasher.u32(static_cast<std::uint32_t>(index));
        hasher.bytes(value);
    }
    style.cache_digest(hasher.value());
    // `cache_digest` maps a zero digest to one, so read it back rather than returning the raw
    // value: every caller must agree on the stored form.
    return style.cached_digest();
}

} // namespace tessera::ui::style
