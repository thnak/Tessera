// SPDX-License-Identifier: MIT
//
// Layout-input signature: a single allocation-free hash over the inputs `layout_engine.cpp` turns
// into Yoga styles and measured boxes. Kept separate from the scheduler so tests can assert the
// inclusion list directly.

#include "tessera/ui/layout/layout_signature.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tessera::ui::layout {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

// Exactly the computed properties read by `detail::apply_style` (src/ui/layout/layout_style.cpp)
// and `text::text_style_from` (src/ui/text/text_measure.cpp), except the paint-only
// `text-overflow`. Sorted by byte value so binary search and a compile-time order check apply.
constexpr std::array<std::string_view, 62> kLayoutProperties = {{
    "align-content",
    "align-items",
    "align-self",
    "aspect-ratio",
    "border-bottom-style",
    "border-bottom-width",
    "border-inline-end-style",
    "border-inline-end-width",
    "border-inline-start-style",
    "border-inline-start-width",
    "border-left-style",
    "border-left-width",
    "border-right-style",
    "border-right-width",
    "border-top-style",
    "border-top-width",
    "box-sizing",
    "column-gap",
    "direction",
    "display",
    "field-sizing",
    "flex-basis",
    "flex-direction",
    "flex-grow",
    "flex-shrink",
    "flex-wrap",
    "font-size",
    "font-style",
    "font-weight",
    "height",
    "inset-inline-end",
    "inset-inline-start",
    "justify-content",
    "left",
    "letter-spacing",
    "line-height",
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
    "text-transform",
    "top",
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

// Framing bytes keep the hash unambiguous: ids, property values and text are all length-prefixed
// and every node is delimited, so shifting bytes between fields changes the signature.
constexpr std::uint8_t kNodeBegin = 0x4E;
constexpr std::uint8_t kProperty = 0x50;
constexpr std::uint8_t kText = 0x54;
constexpr std::uint8_t kNodeEnd = 0x45;

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

    void u64(std::uint64_t value) noexcept {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
        }
    }

    void f32(float value) noexcept {
        u32(std::bit_cast<std::uint32_t>(value));
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

// Returns the index of `name` in the layout property set, or kLayoutProperties.size() when the
// property is paint-only/unknown and must not invalidate a layout.
std::size_t layout_property_index(std::string_view name) noexcept {
    const auto found = std::lower_bound(kLayoutProperties.begin(), kLayoutProperties.end(), name);
    if (found == kLayoutProperties.end() || *found != name) {
        return kLayoutProperties.size();
    }
    return static_cast<std::size_t>(found - kLayoutProperties.begin());
}

void hash_node(const LayoutInput& input, Hasher& hasher) noexcept {
    hasher.u8(kNodeBegin);
    hasher.u64(input.id);
    hasher.u32(static_cast<std::uint32_t>(input.children.size()));

    // `ComputedStyle` owns a sorted map, so this walk is deterministic regardless of insertion
    // order. Only the layout set is folded in; a paint-only change leaves the signature intact.
    for (const auto& [name, value] : input.style.properties()) {
        const std::string_view name_view = name;
        const std::size_t index = layout_property_index(name_view);
        if (index == kLayoutProperties.size()) {
            continue;
        }
        hasher.u8(kProperty);
        hasher.u32(static_cast<std::uint32_t>(index));
        hasher.bytes(value);
    }

    // Text drives Yoga's measure callback, so it is a layout input even when the surrounding box
    // has fixed dimensions. Including it can only cost a redundant reflow, never a wrong reuse.
    hasher.u8(kText);
    hasher.bytes(input.text);

    for (const LayoutInput& child : input.children) {
        hash_node(child, hasher);
    }
    hasher.u8(kNodeEnd);
}

} // namespace

std::uint64_t layout_signature(const LayoutInput& root, const LayoutViewport& viewport) noexcept {
    Hasher hasher;
    hasher.f32(viewport.width);
    hasher.f32(viewport.height);
    hasher.f32(viewport.dpi_scale);
    hash_node(root, hasher);
    return hasher.value();
}

} // namespace tessera::ui::layout
