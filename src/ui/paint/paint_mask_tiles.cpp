// SPDX-License-Identifier: MIT

#include "paint_mask_tiles.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::ui::paint::detail {
namespace {

using style::MaskLength;
using style::MaskRepeatKind;

struct Span {
    float start = 0.0f;
    float size = 0.0f;
};

struct Axis {
    std::array<Span, kMaxMaskTiles + 1> spans{};
    std::size_t count = 0;
    bool too_many = false; // more spans than the array holds; the first ones are kept
};

float resolve_length(const MaskLength& length, float area) {
    switch (length.kind) {
    case MaskLength::Kind::Px:
        return length.value;
    case MaskLength::Kind::Percent:
        return length.value * area;
    case MaskLength::Kind::Auto:
    default:
        return area;
    }
}

// The tiles along one axis for a tile of `tile` in an area of `area`, offset `offset` from the
// area's start (CSS Backgrounds 3 section 3.5 for repeat/space/round, 3.6 for position).
Axis axis_spans(float area, float tile, float fraction, float px, MaskRepeatKind kind) {
    Axis axis;
    if (kind == MaskRepeatKind::Round) {
        // Rescale so a whole number of tiles fills the area, then repeat as usual.
        tile = area / std::max(1.0f, std::round(area / tile));
    }
    const auto push = [&axis](float start, float size) {
        if (axis.count == axis.spans.size()) {
            axis.too_many = true;
            return;
        }
        axis.spans[axis.count++] = Span{start, size};
    };
    float offset = (area - tile) * fraction + px;
    if (kind == MaskRepeatKind::Space) {
        const float fit = std::floor(area / tile);
        if (fit >= 2.0f) {
            // Whole tiles, the leftover shared evenly between them; position does not apply.
            const float gap = (area - fit * tile) / (fit - 1.0f);
            for (float index = 0.0f; index < fit && !axis.too_many; index += 1.0f) {
                push(index * (tile + gap), tile);
            }
            return axis;
        }
        kind = MaskRepeatKind::NoRepeat; // fewer than two fit: one tile at the position
    }
    if (kind == MaskRepeatKind::NoRepeat) {
        push(offset, tile);
        return axis;
    }
    // Back up to the first tile that still reaches the area's start.
    offset -= tile * std::ceil(offset / tile);
    for (float start = offset; start < area && !axis.too_many; start += tile) {
        push(start, tile);
    }
    return axis;
}

} // namespace

MaskTiling compute_mask_tiles(const rhi::Rect& box, const style::MaskSize& size,
                              const style::MaskPosition& position,
                              const style::MaskRepeat& repeat) {
    MaskTiling out;
    const float tile_w =
        (size.cover || size.contain) ? box.width : resolve_length(size.width, box.width);
    const float tile_h =
        (size.cover || size.contain) ? box.height : resolve_length(size.height, box.height);
    if (!(tile_w > 0.0f) || !(tile_h > 0.0f) || !(box.width > 0.0f) || !(box.height > 0.0f)) {
        return out;
    }
    const Axis xs = axis_spans(box.width, tile_w, position.x.fraction, position.x.px, repeat.x);
    const Axis ys = axis_spans(box.height, tile_h, position.y.fraction, position.y.px, repeat.y);
    out.truncated = xs.too_many || ys.too_many || xs.count * ys.count > kMaxMaskTiles;
    for (std::size_t row = 0; row < ys.count && out.count < kMaxMaskTiles; ++row) {
        for (std::size_t column = 0; column < xs.count && out.count < kMaxMaskTiles; ++column) {
            const Span& x = xs.spans[column];
            const Span& y = ys.spans[row];
            out.tiles[out.count++] = rhi::Rect{box.x + x.start, box.y + y.start, x.size, y.size};
        }
    }
    return out;
}

bool tiles_within(const MaskTiling& tiling, const rhi::Rect& area) {
    constexpr float kEpsilon = 0.001f;
    return std::all_of(tiling.tiles.begin(), tiling.tiles.begin() + tiling.count,
                       [&](const rhi::Rect& tile) {
                           return tile.x >= area.x - kEpsilon && tile.y >= area.y - kEpsilon &&
                                  tile.x + tile.width <= area.x + area.width + kEpsilon &&
                                  tile.y + tile.height <= area.y + area.height + kEpsilon;
                       });
}

} // namespace tessera::ui::paint::detail
