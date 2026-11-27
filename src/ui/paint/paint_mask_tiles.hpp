// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/style/mask_layers.hpp"

#include <array>
#include <cstddef>

// Tile geometry of one mask layer (issue #617): where the layer's image is painted inside the mask
// positioning area (the border box) for a `mask-size`, `mask-position` and `mask-repeat`. Pure
// arithmetic, so it is tested without a sink.
namespace tessera::ui::paint::detail {

// A repeat that would need more tiles than this paints no tile at all: every tile is one gradient
// (up to 1024 solid quads), so an unbounded count would be an unbounded frame cost.
inline constexpr std::size_t kMaxMaskTiles = 256;

struct MaskTiling {
    std::array<rhi::Rect, kMaxMaskTiles> tiles{};
    std::size_t count = 0;
    // True when more than `kMaxMaskTiles` would be needed; `count` is then 0.
    bool too_many = false;
    // True when some tile reaches outside the box, so the draws need a clip to the box.
    bool overflows_box = false;
    // True for the initial geometry (one tile, the whole box): the caller can paint it as before.
    bool whole_box = false;
};

// `box` is the positioning area. Gradients have no intrinsic size, so `auto`, `cover` and
// `contain` resolve to the box (CSS Images 3 section 5).
[[nodiscard]] MaskTiling compute_mask_tiles(const rhi::Rect& box, const style::MaskSize& size,
                                            const style::MaskPosition& position,
                                            const style::MaskRepeat& repeat);

} // namespace tessera::ui::paint::detail
