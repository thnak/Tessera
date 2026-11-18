// SPDX-License-Identifier: MIT
//
// Texture size arithmetic and the mip-chain upload entry point of the Vulkan backend. No Vulkan
// handles live here beyond the recorder's signature, so the arithmetic is plain integer code and
// vulkan_internal.hpp stays within its size budget.

#pragma once

#include "tessera/rendering/rhi/device.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <volk.h>

namespace tessera::rendering::vulkan {

inline std::size_t bytes_per_pixel(rhi::TextureFormat format) {
    switch (format) {
    case rhi::TextureFormat::R8G8B8A8Unorm:
        return 4;
    case rhi::TextureFormat::R8Unorm:
        return 1;
    case rhi::TextureFormat::B8G8R8A8Unorm:
        return 4;
    case rhi::TextureFormat::R8G8B8A8Srgb:
        return 4;
    case rhi::TextureFormat::B8G8R8A8Srgb:
        return 4;
    }
    return 0;
}

// Estimated device bytes of a texture (extent x texel size x samples), summed over `levels` mip
// levels; each level halves both axes down to 1.
inline std::int64_t texture_bytes(rhi::Extent2D extent, rhi::TextureFormat format,
                                  std::uint32_t sample_count, std::uint32_t levels = 1) {
    std::int64_t texels = 0;
    for (std::uint32_t level = 0; level < levels; ++level) {
        const std::int64_t width = std::max<std::int64_t>(1, extent.width >> level);
        const std::int64_t height = std::max<std::int64_t>(1, extent.height >> level);
        texels += width * height;
    }
    return texels * static_cast<std::int64_t>(bytes_per_pixel(format)) * sample_count;
}

// A multisampled target owns the 1-sample resolve image as well as the multisample render image.
inline std::int64_t render_target_bytes(rhi::Extent2D extent, rhi::TextureFormat format,
                                        std::uint32_t sample_count) {
    return texture_bytes(extent, format, 1) +
           (sample_count > 1 ? texture_bytes(extent, format, sample_count) : 0);
}

// Records the upload of a tightly packed base level and the downsample chain behind it (RHI 1.6,
// vulkan_mipmaps.cpp): the whole image goes UNDEFINED -> TRANSFER_DST, level 0 is copied from
// `staging`, each level is blitted (linear) from the one above, and every level ends in
// SHADER_READ_ONLY_OPTIMAL.
void record_texture_upload_with_mips(VkCommandBuffer command_buffer, VkImage image,
                                     VkBuffer staging, rhi::Extent2D extent,
                                     std::uint32_t row_length_texels, std::uint32_t levels);

} // namespace tessera::rendering::vulkan
