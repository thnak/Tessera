// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/device.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace tessera::rendering::vulkan {

std::unique_ptr<rhi::Device> create_vulkan_device(const rhi::DeviceDesc& desc);

// Cumulative per-step wall time of `create_texture` (issue #697, work item 1): a direct measurement
// of where a texture creation spends its time, so a device-memory policy is built on a timer and
// not on a correlation with block growth. Nanoseconds, since the device was created; `*_max` is the
// worst single call. Reading it is cheap and never touches the GPU; the counters are relaxed
// atomics bumped around the three steps that can be slow (the other steps, view, sampler and
// descriptor, were measured at under 0.02 ms on both GPUs the issue tested).
struct TextureCreateTimings {
    std::uint64_t calls = 0;
    std::uint64_t image_ns = 0; // vmaCreateImage: device-memory allocation
    std::uint64_t image_ns_max = 0;
    std::uint64_t staging_write_ns = 0; // the memcpy into a fresh staging buffer
    std::uint64_t staging_write_ns_max = 0;
    std::uint64_t submit_ns = 0; // recording and queueing the upload
    std::uint64_t submit_ns_max = 0;
};

// The timings of `device` when it is a Vulkan device, else nullopt.
[[nodiscard]] std::optional<TextureCreateTimings> texture_create_timings(const rhi::Device& device);

} // namespace tessera::rendering::vulkan
