// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/layout/layout_engine.hpp"

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

} // namespace tessera::ui::layout
