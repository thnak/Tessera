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

// A layer paints at most this many tiles: every tile is one gradient, so an unbounded repeat would
// be an unbounded frame cost. A repeat that needs more draws the first ones (row-major) and
// reports the cut in `MaskTiling::truncated`, so the mask degrades instead of vanishing.
inline constexpr std::size_t kMaxMaskTiles = 256;

struct MaskTiling {
    std::array<rhi::Rect, kMaxMaskTiles> tiles{};
    std::size_t count = 0;
    // True when more than `kMaxMaskTiles` were needed: `tiles` holds the first ones only.
    bool truncated = false;
};

// True when every tile lies inside `area`, so the draws need no clip to it.
[[nodiscard]] bool tiles_within(const MaskTiling& tiling, const rhi::Rect& area);

// `box` is the positioning area (`mask-origin`). Gradients have no intrinsic size, so `auto`,
// `cover` and `contain` resolve to the box (CSS Images 3 section 5).
[[nodiscard]] MaskTiling compute_mask_tiles(const rhi::Rect& box, const style::MaskSize& size,
                                            const style::MaskPosition& position,
                                            const style::MaskRepeat& repeat);

} // namespace tessera::ui::paint::detail
