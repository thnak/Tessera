// SPDX-License-Identifier: MIT
//
// PaintData raster-image payload emission. Placement, content box and tint resolution are shared
// with the vector payload (paint_content.hpp), so text/vector/image cannot drift: the image paints
// after the node's background/borders, inside the active overflow/scroll clip, before its children.
// All decode/upload work belongs to the sink.

#include "paint_content.hpp"
#include "paint_image.hpp"

namespace tessera::ui::paint::detail {

void paint_image_content(const Node& node, const style::ComputedStyle& style,
                         const rhi::Rect& border_box, float opacity, PaintSink& sink,
                         PaintStats& stats) {
    const auto& data = node.paint_data();
    if (!data.has_value() || data->kind != PaintKind::Image) {
        return;
    }
    const PaintImageStyle& payload = data->image;
    // Names longer than the cap are ignored (truncation would silently paint a different asset).
    if (payload.asset.empty() || payload.asset.size() > PaintData::kMaxAssetBytes) {
        return;
    }
    const std::optional<rhi::Rect> rect = content_box(style, border_box);
    if (!rect.has_value()) {
        return;
    }
    const std::optional<rhi::Color> tint = content_tint(payload.tint, payload.opacity, opacity);
    if (!tint.has_value()) {
        return;
    }

    ImageDraw draw;
    draw.rect = *rect;
    draw.asset = payload.asset;
    draw.raster_width = payload.raster_width;
    draw.raster_height = payload.raster_height;
    draw.scale = payload.scale;
    draw.decode_original = payload.decode_original;
    draw.tint = *tint;
    sink.draw_image(draw);
    ++stats.images;
}

} // namespace tessera::ui::paint::detail
