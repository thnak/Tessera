// SPDX-License-Identifier: MIT

#pragma once

#include "scene_prepare.hpp"
#include "scene_preview.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// GPU half of the in-memory preview (issue #167, P3 of docs/design/reference-equantic-ui.md section
// 7.4). `scene_preview.hpp` stops at the scene's bounds on purpose; this file is what turns the
// same laid-out tree into RHI commands, so the browser canvas and the native CLI run one
// implementation over whatever `rhi::Device` they were handed (WebGPU in the browser, null or
// Vulkan natively).
//
// Nothing here allocates a device or chooses a backend: the caller owns the device, which is why
// the Wasm module can pass the WebGPU device the page created and a test can pass the null backend
// and assert the recorded command stream.
//
// Two deliberate limits, both reported in `SceneFrameResult` instead of hidden:
//
//  - **No text.** Drawing a glyph run needs the FreeType/HarfBuzz stack plus embedded font bytes;
//    the preview module ships no font (a subset plus its licensing is its own decision), so a node
//    whose payload is text is painted as its box only and counted in `text_skipped`. The parity
//    scene set of tests/gpu/backends has no glyph runs, so the render comparison this stage exists
//    for is unaffected.
//  - **No vector/image assets.** A preview source has no embedded AssetBundle, so
//  `PaintKind::Vector`
//    and `PaintKind::Image` payloads are counted in `vector_skipped`/`image_skipped` and not drawn.
//    As with text, this is a missing input rather than a missing capability.
namespace tessera::compiler::codegen::cli {

namespace rhi = tessera::rendering::rhi;

// One rendered frame: tightly packed RGBA8, top-left origin, `width * height * 4` bytes.
struct SceneFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

struct SceneFrameResult {
    // Non-empty when the frame could not be produced.
    std::string error;
    SceneFrame frame;
    // False when this backend cannot read pixels back synchronously (WebGPU in a browser). The draw
    // counters below are still valid - the command stream was recorded and submitted - so a browser
    // caller gates on those and reports the pixel comparison as the browser lane's job.
    bool pixels_available = false;
    // Paint-pass counters, so a caller can tell "painted nothing" from "had nothing to paint".
    std::size_t nodes = 0;
    std::size_t backgrounds = 0;
    std::size_t borders = 0;
    std::size_t texts = 0;
    std::size_t text_skipped = 0;
    std::size_t vectors_skipped = 0;
    std::size_t images_skipped = 0;
    std::size_t backdrops_skipped = 0;
    // Renderer counters for the frame's single pass.
    std::uint32_t draw_calls = 0;
    std::uint32_t vertex_count = 0;
};

// The clear colour of a preview frame. It is the app window background (`frame_clear_color()` of
// src/app/window_internal.hpp) so a previewed screen and the native window of the same stylesheet
// start from the same pixel; a preview that cleared to black would differ everywhere the stylesheet
// paints nothing.
[[nodiscard]] rhi::Color scene_frame_clear_color() noexcept;

// The pipeline description the batched 2D renderer needs, expressed only through RHI types: the
// logical shader id (`gpu::kBatched2dShaderId`, ADR-049) plus the shared vertex layout and
// push-constant size. No SPIR-V and no Vulkan header, so this description is creatable by WebGPU,
// which embeds its WGSL twin and has no GLSL output at all.
//
// A Vulkan caller cannot use it as-is: that backend builds its pipeline from SPIR-V words, so it
// passes `vulkan::batched2d_pipeline_desc()` to `scene_frame_render()` instead. That keeps this
// translation unit free of `vulkan/` and of `<webgpu/webgpu.h>`, which is what lets one driver
// compile on the host lanes and link into the browser module unchanged.
[[nodiscard]] rhi::PipelineDesc
scene_frame_pipeline_desc(rhi::TextureFormat format = rhi::TextureFormat::R8G8B8A8Unorm,
                          std::uint32_t sample_count = 1);

// Paints a laid-out scene into `target`: the half of `scene_frame_render()` that records commands
// and touches no pixels. It exists separately because the browser's draw target is a **canvas
// texture the host owns** - surface-backed, not something this RHI's `create_render_target()`
// produced - so the browser path needs "paint into this target" without the offscreen+readback
// half.
//
// `target` and `device` must outlive the call, and `desc` must describe `target`. `pipeline` is
// used as given when it carries a vertex layout; otherwise `scene_frame_pipeline_desc()` supplies
// one for `desc.format`.
[[nodiscard]] SceneFrameResult scene_frame_paint(const PreparedScene& scene, rhi::Device& device,
                                                 rhi::RenderTarget& target,
                                                 const rhi::RenderTargetDesc& desc,
                                                 const rhi::PipelineDesc& pipeline = {});

// Evaluates `text` exactly as `scene_preview_document()` does (same stages, same options, same
// salt) and then paints the resulting tree with `ui::paint::paint_scene()` through the batched 2D
// renderer onto an offscreen target of `extent`, reading the pixels back through the RHI.
//
// `device` must outlive the call. `extent` is the *physical* target size; pass the layout viewport
// multiplied by the page's device-pixel ratio, because a preview that laid out at one size and
// rendered at another would disagree with the native window.
//
// `pipeline` defaults to `scene_frame_pipeline_desc()` - the shader-id description that WebGPU (and
// the null backend) accept. A backend that builds from SPIR-V must pass its own description, which
// is why the parameter exists rather than being constructed inside.
//
// Deterministic for a given (source, options, component, extent, pipeline) on a deterministic
// device: the null backend records the same command stream every run, and a software Vulkan device
// produces the same bytes. `pixels_available` is false for a backend that cannot map synchronously
// (WebGPU in a browser); the command stream is still recorded, the pixels are simply absent, and
// the caller gates on the counters instead of comparing an empty image.
[[nodiscard]] SceneFrameResult scene_frame_render(std::string_view text,
                                                  const ScenePreviewOptions& options,
                                                  rhi::Device& device, rhi::Extent2D extent,
                                                  std::string_view component = {},
                                                  const rhi::PipelineDesc& pipeline = {});

} // namespace tessera::compiler::codegen::cli
