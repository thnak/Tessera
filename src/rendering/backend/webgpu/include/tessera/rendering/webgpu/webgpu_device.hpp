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

} // namespace tessera::rendering::webgpu
