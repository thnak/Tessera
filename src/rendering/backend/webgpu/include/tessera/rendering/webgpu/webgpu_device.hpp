// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/webgpu/shader_registry.hpp"

#include <memory>
#include <string_view>

namespace tessera::rendering::webgpu {

// Wraps a ready `WGPUDevice` and `WGPUQueue` (passed as opaque pointers so this header stays free
// of webgpu.h) in an `rhi::Device`. The browser host requests the adapter and device
// asynchronously and owns the canvas surface; the backend takes a reference on both handles.
//
// Only available when the `tessera_webgpu` target is built (Emscripten, or TESSERA_BUILD_WEBGPU).
[[nodiscard]] std::unique_ptr<rhi::Device> create_device(void* wgpu_device, void* wgpu_queue);

// Registers WGSL under the `shader_id` a `PipelineDesc` will carry (built-in ids are already
// registered). `device` must come from `create_device`. Returns false for any other device or an
// empty id.
bool register_shader(rhi::Device& device, std::string_view shader_id, const WgslProgram& program);

// Wraps the current canvas texture view as an RHI render target (issue #167, P3 of
// docs/design/reference-equantic-ui.md section 7.4). The page asks the configured canvas surface
// for `getCurrentTexture()` once per frame and hands the view here; the returned target owns that
// reference and releases it when the frame is done.
//
// This is the drawing half of a swapchain, not a swapchain: `Device::create_swapchain` stays
// `NotSupported` because a canvas surface is configured by the page (device, format, alpha mode,
// size) and this backend's job is only to render into whatever image the page already holds. The
// target reports `presentable() == true`, so the RHI refuses to use it as a copy source or a
// sampled bind - the same rule a Vulkan swapchain image follows.
//
// `format` must be the canvas format the page configured, or WebGPU rejects the pipeline at draw
// time; it is a parameter rather than a query because the format is the page's decision (the usual
// choice is the preferred canvas format, which is BGRA8 on most platforms).
[[nodiscard]] std::unique_ptr<rhi::RenderTarget>
create_canvas_target(void* texture_view, rhi::Extent2D extent, rhi::TextureFormat format);

} // namespace tessera::rendering::webgpu
