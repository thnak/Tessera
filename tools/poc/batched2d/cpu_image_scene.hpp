// SPDX-License-Identifier: MIT
//
// Deterministic CPU raster-image scene shared by the batched2d POC (`--backend cpu --cpu-image`)
// and the Vulkan-free `batched2d_cpu_image_golden` test. PaintData image payloads go through the
// normal paint pass into CpuPaintSink: embedded PNG decode -> CpuCanvas::blit_texture. Two nodes
// share one image (one decode), one node is tinted/translucent, one is clipped by an overflow
// parent, and one names a missing asset that must stay background. PNG only: JPEG decode uses a
// floating-point IDCT whose last bits may differ across compilers, so JPEG is covered by decode
// unit tests rather than a tolerance-0 cross-platform golden (documented).

#pragma once

#include "cpu_paint_sink.hpp"
#include "cpu_scene_styles.hpp"
#include "tessera/rendering/image/image_decode_cache.hpp"
#include "tessera/rendering/software/cpu_canvas.hpp"
#include "tessera/ui/assets/asset_bundle.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::poc::batched2d {

struct CpuImageSceneResult {
    bool built = false;      // false when no image can be decoded (no fixtures/backend)
    std::uint32_t draws = 0; // image draws emitted (CPU sink))
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Paints the scene into `canvas` (cleared to #0B1220 by this function). `bundle` and `decoded` must
// outlive the call. Images are decoded once through `decoded`; a failed/missing asset is a no-op.
[[nodiscard]] inline CpuImageSceneResult
build_cpu_image_screen(rendering::software::CpuCanvas& canvas,
                       const ui::assets::AssetBundle& bundle,
                       rendering::image::ImageDecodeCache& decoded) {
    using namespace tessera;
    using namespace tessera::rendering;
    CpuImageSceneResult result;
    result.width = canvas.width();
    result.height = canvas.height();

    CpuPaintSink sink(canvas, nullptr, nullptr, {}, CpuImageConfig{&bundle, &decoded, 1.0f});

    const float width = static_cast<float>(canvas.width());
    const float height = static_cast<float>(canvas.height());
    const auto background = rhi::rgba8(11, 18, 32);

    ui::Node root(1);
    root.set_bounds({0, 0, width, height});

    auto& header = root.add_child(std::make_unique<ui::Node>(2));
    header.set_bounds({0, 0, width, 32});
    auto& accent_line = root.add_child(std::make_unique<ui::Node>(3));
    accent_line.set_bounds({0, 32, width, 2});

    const auto add_image = [&root](ui::NodeId id, float x, float y, float w, float h,
                                   std::string_view asset) -> ui::Node& {
        ui::Node& node = root.add_child(std::make_unique<ui::Node>(id));
        node.set_bounds({x, y, w, h});
        ui::PaintData data;
        data.kind = ui::PaintKind::Image;
        data.image.asset = asset;
        node.set_paint_data(std::move(data));
        return node;
    };
    add_image(4, 16, 48, 64, 64, "images/checker.png");
    add_image(5, 96, 48, 64, 64, "images/checker.png");
    // Tinted + translucent copy (tint multiplies channels; payload opacity folds alpha).
    ui::Node& tinted = add_image(6, 16, 120, 32, 32, "images/checker.png");
    {
        ui::PaintData data = *tinted.paint_data();
        data.image.tint = rhi::rgba8(255, 128, 96, 255);
        data.image.opacity = 0.5f;
        tinted.set_paint_data(std::move(data));
    }
    // Missing asset: paints nothing; the background behind it stays visible.
    ui::Node& missing = add_image(7, 96, 120, 64, 32, "images/missing.png");

    CpuSceneStyles styles;
    styles.set(1, cpu_style({{"background-color", "#0B1220"}}));
    styles.set(2, cpu_style({{"background-color", "#0F172A"}}));
    styles.set(3, cpu_style({{"background-color", "#38BDF8"}}));

    // Clipped parent: the child image is larger than the clip, so the cut is visible in the golden.
    ui::Node& clipped_parent = root.add_child(std::make_unique<ui::Node>(8));
    clipped_parent.set_bounds({176, 48, 64, 64});
    styles.set(8, cpu_style({{"background-color", "#1E293B"},
                             {"overflow-x", "hidden"},
                             {"overflow-y", "hidden"}}));
    ui::Node& overflowing = clipped_parent.add_child(std::make_unique<ui::Node>(9));
    overflowing.set_bounds({0, 0, 96, 96});
    {
        ui::PaintData data;
        data.kind = ui::PaintKind::Image;
        data.image.asset = "images/checker.png";
        overflowing.set_paint_data(std::move(data));
    }
    styles.set(9, cpu_style({}));
    (void)missing;
    (void)overflowing;

    canvas.clear(background);
    const ui::paint::PaintStats stats = ui::paint::paint_scene(root, styles, sink);
    result.built = stats.images > 0U || decoded.stats().decodes > 0U;
    result.draws = stats.images;
    return result;
}

} // namespace tessera::poc::batched2d
