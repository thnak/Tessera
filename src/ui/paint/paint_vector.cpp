// SPDX-License-Identifier: MIT
//
// PaintData vector payload emission. The node's background/borders are already painted and the
// active overflow/scroll clip is pushed when this runs (same placement as text runs), so a vector
// asset is clipped and damage-replayed exactly like glyphs. Geometry, content box and tint come
// from paint_content.hpp (shared with the image payload); all GPU work is the sink's job.

#include "paint_content.hpp"
#include "paint_vector.hpp"

namespace tessera::ui::paint::detail {

void paint_vector_content(const Node& node, const style::ComputedStyle& style,
                          const rhi::Rect& border_box, PaintSink& sink, PaintStats& stats) {
    const auto& data = node.paint_data();
    if (!data.has_value() || data->kind != PaintKind::Vector) {
        return;
    }
    const PaintVectorStyle& payload = data->vector;
    // Names longer than the cap are ignored (truncation would silently paint a different asset).
    if (payload.asset.empty() || payload.asset.size() > PaintData::kMaxAssetBytes) {
        return;
    }
    // Content box and tint are shared with the image payload; clipping is already active for the
    // node and children paint after this content.
    const std::optional<rhi::Rect> rect = content_box(style, border_box);
    if (!rect.has_value()) {
        return;
    }
    const std::optional<rhi::Color> tint = content_tint(payload.tint, payload.opacity, style);
    if (!tint.has_value()) {
        return;
    }

    VectorDraw draw;
    draw.rect = *rect;
    draw.asset = payload.asset;
    draw.raster_width = payload.raster_width;
    draw.raster_height = payload.raster_height;
    draw.scale = payload.scale;
    draw.tint = *tint;
    sink.draw_vector(draw);
    ++stats.vectors;
}

} // namespace tessera::ui::paint::detail
