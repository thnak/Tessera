// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

namespace tessera::rendering::vulkan {

// Pipeline identity for the storage-buffer conformance probe (RHI 1.12, ADR-068 slice 1): a
// fullscreen triangle whose fragment stage reads one read-only storage buffer and writes the value
// it found. It exists so `CommandList::bind_storage_buffer` is proven end to end on a real device
// (tests/gpu/conformance/conformance_checks.cpp) instead of only structurally on the null backend.
//
// The descriptor declares `PipelineDesc::storage_buffer`, so `create_pipeline` rejects it on a
// device that reports `supports_storage_buffers() == false`; the caller checks the capability
// first.
[[nodiscard]] rhi::PipelineDesc
storage_probe_pipeline_desc(rhi::TextureFormat color_format = rhi::TextureFormat::R8G8B8A8Unorm);

} // namespace tessera::rendering::vulkan
