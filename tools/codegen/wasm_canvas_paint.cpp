// SPDX-License-Identifier: MIT

#include "preview_text.hpp"
#include "tessera/rendering/webgpu/webgpu_device.hpp"
#include "wasm_canvas_font.hpp"
#include "wasm_canvas_paint.hpp"
#include "wasm_canvas_support.hpp"

#include <exception>
#include <memory>
#include <string>

namespace tessera::compiler::codegen::cli {

namespace rhi = rendering::rhi;
namespace webgpu = rendering::webgpu;

// `clear_canvas` lives in `wasm_canvas_support.cpp` (issue #390), not here: it needs no device
// creation and no backend header, so it is the part of the clear path the host lanes can compile.
// This unit is the Emscripten-only half that decides *when* to clear -- it always passes the canvas
// format, because an undrawn swapchain shows black and the clear must fill the texture the page
// presents.

SceneFrameResult paint_to_canvas(std::string_view source, const ScenePreviewOptions& options,
                                 std::string_view component, rhi::Extent2D extent,
                                 void* canvas_view, rhi::Device& device) {
    SceneFrameResult result;
    if (extent.width == 0 || extent.height == 0) {
        result.error = "paint_to_canvas: the canvas extent must be positive";
        return result;
    }
    // The target takes ownership of the view first, so that no later failure (a bad source) leaks
    // the per-frame reference.
    std::unique_ptr<rhi::RenderTarget> target;
    try {
        target = webgpu::create_canvas_target(canvas_view, extent, kCanvasFormat);
    } catch (const std::exception& failure) {
        result.error = std::string("paint_to_canvas: ") + failure.what();
        return result;
    }
    if (target == nullptr) {
        result.error = "paint_to_canvas: no render target";
        return result;
    }

    const PreparedScene scene = prepare_scene(source, options, component, canvas_measurer());
    if (!scene.error.empty() || scene.root == nullptr) {
        result.error = scene.error.empty() ? "the source produced no scene" : scene.error;
        // The one thing this path still clears on: the page asked for a frame, so it must not be
        // left showing the previous one under a report that says it failed. The live edit avoids
        // this entirely by not submitting a frame at all when the source does not evaluate.
        clear_canvas(device, *target, extent, kCanvasFormat);
        return result;
    }

    rhi::RenderTargetDesc desc;
    desc.extent = extent;
    desc.format = kCanvasFormat;
    desc.clear_color = scene_frame_clear_color();
    desc.load_op = rhi::LoadOp::Clear;
    desc.store_op = rhi::StoreOp::Store;
    SceneFrameResult painted =
        scene_frame_paint(scene, device, *target, desc, {}, canvas_text_stack());
    if (!painted.error.empty()) {
        clear_canvas(device, *target, extent, kCanvasFormat);
    }
    return painted;
}

SceneFrameResult paint_session(CanvasSceneSession& session, rhi::Extent2D extent, void* canvas_view,
                               rhi::Device& device) {
    SceneFrameResult result;
    if (extent.width == 0 || extent.height == 0) {
        result.error = "paint_session_to_canvas: the canvas extent must be positive";
        return result;
    }
    rhi::RenderTargetDesc desc;
    desc.extent = extent;
    desc.format = kCanvasFormat;
    desc.clear_color = scene_frame_clear_color();
    desc.load_op = rhi::LoadOp::Clear;
    desc.store_op = rhi::StoreOp::Store;

    std::unique_ptr<rhi::RenderTarget> target;
    try {
        target = canvas_view != nullptr
                     ? webgpu::create_canvas_target(canvas_view, extent, kCanvasFormat)
                     : device.create_render_target(desc);
    } catch (const std::exception& failure) {
        result.error = std::string("paint_session_to_canvas: ") + failure.what();
        return result;
    }
    if (target == nullptr) {
        result.error = "paint_session_to_canvas: no render target";
        return result;
    }
    result = session.paint(device, *target, desc, {}, canvas_text_stack());
    if (!result.error.empty() && canvas_view != nullptr) {
        clear_canvas(device, *target, extent, kCanvasFormat);
    }
    return result;
}

} // namespace tessera::compiler::codegen::cli
