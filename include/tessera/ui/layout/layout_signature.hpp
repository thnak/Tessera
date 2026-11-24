// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/layout/layout_engine.hpp"

#include <cstddef>
#include <cstdint>

namespace tessera::ui::layout {

// Deterministic FNV-1a signature over every input that can change a `LayoutResult`:
//
//   * tree shape: node id, child count and order, recursively;
//   * per node: the computed longhands that `apply_style` or `text_style_from` read, the measured
//     `text`, and `field-sizing` (it decides whether an empty leaf is measured);
//   * the viewport: width, height and dpi scale.
//
// The pass is allocation-free and performs no string ownership: it walks the already-computed
// `ComputedStyle` map once per node and folds only the property names in the layout set.
//
// Deliberately excluded (changing one must not move a box): paint-only and interaction longhands
// such as background/color/opacity/visibility/transform/border-*-color/border-radius/box-shadow/
// text-overflow/font-family/z-index. `font-family` is excluded because the measure seam receives a
// `text::TextStyle`, which carries no family; adding one later means adding it here too.
//
// Guarantee: for two `LayoutInput` trees and viewports with the same signature, `LayoutEngine`
// produces the same boxes. A `LayoutScheduler` hit is therefore equivalent to a reflow of the same
// inputs; the only gap is the 64-bit hash collision probability (2^-64 for distinct inputs).
[[nodiscard]] std::uint64_t layout_signature(const LayoutInput& root,
                                             const LayoutViewport& viewport) noexcept;

// The same signature, built while the caller walks the tree. A caller that is already visiting
// every node to build the `LayoutInput` (the host's `fill_layout_input`) gets the signature without
// a second pass over nodes that are no longer in cache, which is what the separate walk cost at 12k
// nodes. `layout_signature` is defined through this class, so the two cannot disagree.
//
// Call `enter` once the node's own fields (id, style, text, wrap) are final and before its
// children, then `leave` with the number of children actually kept after the last one. Nodes are
// visited in pre-order, exactly the order `layout_signature` walks `children`.
class LayoutSignatureBuilder {
public:
    explicit LayoutSignatureBuilder(const LayoutViewport& viewport) noexcept;

    void enter(const LayoutInput& node) noexcept;
    void leave(std::size_t child_count) noexcept;

    [[nodiscard]] std::uint64_t value() const noexcept {
        return state_;
    }

private:
    std::uint64_t state_ = 0;
};

// The layout half of one computed style, as a digest: exactly the `kLayoutProperties` entries
// `layout_signature` folds in, and nothing else. Computed once and cached on the style's shared
// entries (`ComputedStyle::cached_digest`), so a second call for an unchanged style is one load.
//
// This is what makes the layout-unchanged gate cheap (issue #735, slice A): comparing a node's
// digest before and after a restyle answers "did this restyle move a box?" without building a
// `LayoutInput` or hashing the tree. A paint-only change leaves the digest identical.
[[nodiscard]] std::uint64_t style_layout_digest(const style::ComputedStyle& style) noexcept;

} // namespace tessera::ui::layout
