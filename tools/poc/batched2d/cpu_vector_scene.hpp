// SPDX-License-Identifier: MIT
//
// Deterministic CPU vector-paint scene shared by the batched2d POC (`--backend cpu --cpu-vector`)
// and the Vulkan-free `batched2d_cpu_vector_golden` test. PaintData vector payloads go through the
// normal paint pass into CpuPaintSink: SVG decode -> CPU rasterize (shared size/scale rule) ->
// textured-quad blit. Two nodes share one icon, one node is tinted/translucent, one sits inside a
// clipped parent, and one names a missing asset that must stay background. No GPU, no timers.

#pragma once

#include "cpu_paint_sink.hpp"
#include "cpu_scene_styles.hpp"
#include "tessera/rendering/software/cpu_canvas.hpp"
#include "tessera/ui/assets/asset_bundle.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::poc::batched2d {

struct CpuVectorSceneResult {
    bool built = false;      // false when the ThorVG CPU backend is not compiled in
    std::uint32_t draws = 0; // successful textured-quad blits
    double raster_ms = 0.0;  // accumulated CPU rasterization time
    double blit_ms = 0.0;    // accumulated blit time
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Paints the scene into `canvas` (which the caller clears when it wants the documented background;
// the function clears it itself with #0B1220). `bundle` must outlive the call. Returns
// `built=false` with a "cpu-vector: skipped (...)" line when the build has no CPU SVG rasterizer.
[[nodiscard]] inline CpuVectorSceneResult
build_cpu_vector_screen(rendering::software::CpuCanvas& canvas,
                        const ui::assets::AssetBundle& bundle) {
    using namespace tessera;
    using namespace tessera::rendering;
    CpuVectorSceneResult result;
    result.width = canvas.width();
    result.height = canvas.height();
    if (!rendering::path::svg_backend_available()) {
        std::cout << "cpu-vector: skipped (ThorVG backend unavailable)\n";
        return result;
    }
    auto rasterizer = rendering::path::make_path_rasterizer(rendering::path::PathBackend::thorvg);
    if (rasterizer == nullptr) {
        std::cout << "cpu-vector: skipped (no CPU path rasterizer)\n";
        return result;
    }
    rendering::path::SvgDocumentCache documents;
    CpuPaintSink sink(canvas, nullptr, nullptr,
                      CpuVectorConfig{&bundle, &documents, rasterizer.get(), 1.0f});

    const float width = static_cast<float>(canvas.width());
    const float height = static_cast<float>(canvas.height());
    const auto background = rhi::rgba8(11, 18, 32);

    ui::Node root(1);
    root.set_bounds({0, 0, width, height});

    auto& header = root.add_child(std::make_unique<ui::Node>(2));
    header.set_bounds({0, 0, width, 32});
    auto& accent_line = root.add_child(std::make_unique<ui::Node>(3));
    accent_line.set_bounds({0, 32, width, 2});

    const auto add_icon = [&root](ui::NodeId id, float x, float y, float size,
                                  std::string_view asset) -> ui::Node& {
        ui::Node& node = root.add_child(std::make_unique<ui::Node>(id));
        node.set_bounds({x, y, size, size});
        ui::PaintData data;
        data.kind = ui::PaintKind::Vector;
        data.vector.asset = asset;
        node.set_paint_data(std::move(data));
        return node;
    };
    add_icon(4, 16, 48, 64, "icons/icon_shapes.svg");
    add_icon(5, 96, 48, 64, "icons/stroke_path.svg");
    add_icon(6, 176, 48, 64, "icons/icon_shapes.svg");
    // Tinted + translucent copy (tint multiplies sampled channels; payload opacity folds alpha).
    ui::Node& tinted = add_icon(7, 16, 120, 32, "icons/icon_shapes.svg");
    {
        ui::PaintData data = *tinted.paint_data();
        data.vector.tint = rhi::rgba8(255, 128, 96, 255);
        data.vector.opacity = 0.5f;
        tinted.set_paint_data(std::move(data));
    }
    // Missing asset: paints nothing; the background behind it stays visible.
    ui::Node& missing = root.add_child(std::make_unique<ui::Node>(8));
    missing.set_bounds({96, 120, 64, 32});
    {
        ui::PaintData data;
        data.kind = ui::PaintKind::Vector;
        data.vector.asset = "icons/missing.svg";
        missing.set_paint_data(std::move(data));
    }

    CpuSceneStyles styles;
    styles.set(1, cpu_style({{"background-color", "#0B1220"}}));
    styles.set(2, cpu_style({{"background-color", "#0F172A"}}));
    styles.set(3, cpu_style({{"background-color", "#38BDF8"}}));

    // Clipped parent: the child vector starts at the parent's origin and is larger than the clip.
    ui::Node& clipped_parent = root.add_child(std::make_unique<ui::Node>(9));
    clipped_parent.set_bounds({176, 48, 64, 64});
    styles.set(9, cpu_style({{"background-color", "#1E293B"},
                             {"overflow-x", "hidden"},
                             {"overflow-y", "hidden"}}));
    ui::Node& overflowing = clipped_parent.add_child(std::make_unique<ui::Node>(10));
    overflowing.set_bounds({0, 0, 96, 96});
    {
        ui::PaintData data;
        data.kind = ui::PaintKind::Vector;
        data.vector.asset = "icons/icon_shapes.svg";
        overflowing.set_paint_data(std::move(data));
    }
    styles.set(10, cpu_style({}));

    canvas.clear(background);
    (void)ui::paint::paint_scene(root, styles, sink);
    result.built = true;
    result.draws = sink.vector_draws();
    result.raster_ms = sink.raster_ms();
    result.blit_ms = sink.blit_ms();
    return result;
}

} // namespace tessera::poc::batched2d
