// SPDX-License-Identifier: MIT
//
// Layout-input signature: a single allocation-free hash over the inputs `layout_engine.cpp` turns
// into Yoga styles and measured boxes. Kept separate from the scheduler so tests can assert the
// inclusion list directly.

#include "tessera/ui/layout/layout_signature.hpp"
#include "tessera/ui/style/layout_digest.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tessera::ui::layout {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

// Framing bytes keep the hash unambiguous: ids, property values and text are all length-prefixed
// and every node is delimited, so shifting bytes between fields changes the signature.
constexpr std::uint8_t kNodeBegin = 0x4E;
constexpr std::uint8_t kText = 0x54;
constexpr std::uint8_t kTextWrap = 0x57;
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

void hash_node(const LayoutInput& input, Hasher& hasher) noexcept {
    hasher.u8(kNodeBegin);
    hasher.u64(input.id);
    hasher.u32(static_cast<std::uint32_t>(input.children.size()));

    // `ComputedStyle` owns a sorted map, so this walk is deterministic regardless of insertion
    // order. Only the layout set is folded in; a paint-only change leaves the signature intact.
    // The layout part of the style is hashed on its own and cached with the style's shared
    // entries, so a node whose style did not change costs one load instead of a pass over ~100
    // names (issue #711). The property set and the hash live in the style module
    // (`style::layout_digest`), because `tessera_layout` depends on `tessera_style` and not the
    // other way round; the layout-unchanged gate reads the same digest (issue #735).
    hasher.u64(style::layout_digest(input.style));

    // Text drives Yoga's measure callback, so it is a layout input even when the surrounding box
    // has fixed dimensions. Including it can only cost a redundant reflow, never a wrong reuse.
    hasher.u8(kText);
    hasher.bytes(input.text);

    // Paint's wrap opt-in changes no box, but it decides `LayoutBox::text_fit`, which is part of
    // the result the scheduler reuses (issue #490). Folding it in means a wrap flip re-measures
    // instead of reusing a stale fit fact.
    hasher.u8(kTextWrap);
    hasher.u8(input.text_wrap ? 1u : 0u);

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
