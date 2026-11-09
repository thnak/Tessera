// SPDX-License-Identifier: MIT
//
// PaintData vector payload emission. The node's background/borders are already painted and the
// active overflow/scroll clip is pushed when this runs (same placement as text runs), so a vector
// asset is clipped and damage-replayed exactly like glyphs. All GPU work is the sink's job: this
// file only resolves geometry, opacity and tint into a VectorDraw.

#include "paint_properties.hpp"
#include "paint_vector.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::ui::paint::detail {
namespace {

constexpr rhi::Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};

} // namespace

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
    if (border_box.width <= 0.0f || border_box.height <= 0.0f) {
        return;
    }

    const float opacity = detail::opacity_value(style) * std::clamp(payload.opacity, 0.0f, 1.0f);
    if (opacity <= 0.0f) {
        return;
    }

    // Content box = border box inset by padding, matching text runs. Clipping is already active for
    // the node; children paint after this content.
    const float padding_left = detail::length_value(style, "padding-left").value_or(0.0f);
    const float padding_right = detail::length_value(style, "padding-right").value_or(0.0f);
    const float padding_top = detail::length_value(style, "padding-top").value_or(0.0f);
    const float padding_bottom = detail::length_value(style, "padding-bottom").value_or(0.0f);
    const float content_width = std::max(0.0f, border_box.width - padding_left - padding_right);
    const float content_height = std::max(0.0f, border_box.height - padding_top - padding_bottom);
    if (content_width <= 0.0f || content_height <= 0.0f) {
        return;
    }

    rhi::Color tint = payload.tint.value_or(kWhite);
    tint.a = std::clamp(tint.a, 0.0f, 1.0f) * opacity;
    if (tint.a <= 0.0f) {
        return;
    }

    VectorDraw draw;
    draw.rect = rhi::Rect{border_box.x + padding_left, border_box.y + padding_top, content_width,
                          content_height};
    draw.asset = payload.asset;
    draw.raster_width = payload.raster_width;
    draw.raster_height = payload.raster_height;
    draw.scale = payload.scale;
    draw.tint = tint;
    sink.draw_vector(draw);
    ++stats.vectors;
}

} // namespace tessera::ui::paint::detail
