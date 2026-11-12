// SPDX-License-Identifier: MIT

#include "tessera/rendering/webgpu/webgpu_mapping.hpp"

namespace tessera::rendering::webgpu {

WgpuTextureFormat texture_format(rhi::TextureFormat format) {
    switch (format) {
    case rhi::TextureFormat::R8G8B8A8Unorm:
        return WgpuTextureFormat::Rgba8Unorm;
    case rhi::TextureFormat::R8Unorm:
        return WgpuTextureFormat::R8Unorm;
    case rhi::TextureFormat::B8G8R8A8Unorm:
        return WgpuTextureFormat::Bgra8Unorm;
    case rhi::TextureFormat::R8G8B8A8Srgb:
        return WgpuTextureFormat::Rgba8UnormSrgb;
    case rhi::TextureFormat::B8G8R8A8Srgb:
        return WgpuTextureFormat::Bgra8UnormSrgb;
    }
    return WgpuTextureFormat::Rgba8Unorm;
}

std::string_view texture_format_name(WgpuTextureFormat format) {
    switch (format) {
    case WgpuTextureFormat::Rgba8Unorm:
        return "rgba8unorm";
    case WgpuTextureFormat::R8Unorm:
        return "r8unorm";
    case WgpuTextureFormat::Bgra8Unorm:
        return "bgra8unorm";
    case WgpuTextureFormat::Rgba8UnormSrgb:
        return "rgba8unorm-srgb";
    case WgpuTextureFormat::Bgra8UnormSrgb:
        return "bgra8unorm-srgb";
    }
    return "";
}

std::uint32_t bytes_per_pixel(rhi::TextureFormat format) {
    return format == rhi::TextureFormat::R8Unorm ? 1u : 4u;
}

std::uint32_t buffer_usage_flags(rhi::BufferUsage usage) {
    namespace bits = buffer_usage_bits;
    switch (usage) {
    case rhi::BufferUsage::Vertex:
        return bits::kVertex | bits::kCopyDst;
    case rhi::BufferUsage::Index:
        return bits::kIndex | bits::kCopyDst;
    case rhi::BufferUsage::Uniform:
        return bits::kUniform | bits::kCopyDst;
    // WebGPU forbids MAP_WRITE with anything but COPY_SRC, and MAP_READ with anything but COPY_DST.
    case rhi::BufferUsage::Staging:
        return bits::kMapWrite | bits::kCopySrc;
    case rhi::BufferUsage::Readback:
        return bits::kMapRead | bits::kCopyDst;
    }
    return 0;
}

std::uint32_t sampled_texture_usage() {
    return texture_usage_bits::kTextureBinding | texture_usage_bits::kCopyDst;
}

std::uint32_t render_target_usage(std::uint32_t sample_count) {
    namespace bits = texture_usage_bits;
    if (sample_count > 1) {
        return bits::kRenderAttachment;
    }
    return bits::kRenderAttachment | bits::kTextureBinding | bits::kCopySrc;
}

WgpuFilterMode filter_mode(rhi::Filter filter) {
    return filter == rhi::Filter::Linear ? WgpuFilterMode::Linear : WgpuFilterMode::Nearest;
}

WgpuAddressMode address_mode(rhi::AddressMode mode) {
    return mode == rhi::AddressMode::Repeat ? WgpuAddressMode::Repeat
                                            : WgpuAddressMode::ClampToEdge;
}

// DontCare maps to Clear: the contents are undefined to the caller, and clearing is the cheapest
// defined behavior on tiled GPUs (Load would force a read of the previous contents).
WgpuLoadOp load_op(rhi::LoadOp op) {
    return op == rhi::LoadOp::Load ? WgpuLoadOp::Load : WgpuLoadOp::Clear;
}

WgpuStoreOp store_op(rhi::StoreOp op) {
    return op == rhi::StoreOp::Store ? WgpuStoreOp::Store : WgpuStoreOp::Discard;
}

WgpuVertexFormat vertex_format(rhi::VertexFormat format) {
    switch (format) {
    case rhi::VertexFormat::Float:
        return WgpuVertexFormat::Float32;
    case rhi::VertexFormat::Float2:
        return WgpuVertexFormat::Float32x2;
    case rhi::VertexFormat::Float3:
        return WgpuVertexFormat::Float32x3;
    case rhi::VertexFormat::Float4:
        return WgpuVertexFormat::Float32x4;
    case rhi::VertexFormat::UByte4Norm:
        return WgpuVertexFormat::Unorm8x4;
    }
    return WgpuVertexFormat::Float32;
}

std::uint32_t vertex_format_size(rhi::VertexFormat format) {
    switch (format) {
    case rhi::VertexFormat::Float:
        return 4;
    case rhi::VertexFormat::Float2:
        return 8;
    case rhi::VertexFormat::Float3:
        return 12;
    case rhi::VertexFormat::Float4:
        return 16;
    case rhi::VertexFormat::UByte4Norm:
        return 4;
    }
    return 0;
}

// Same factors as the Vulkan backend.
WgpuBlendState blend_state(rhi::BlendMode mode) {
    WgpuBlendState state;
    if (mode != rhi::BlendMode::Alpha) {
        return state;
    }
    state.enabled = true;
    state.color = {WgpuBlendFactor::SrcAlpha, WgpuBlendFactor::OneMinusSrcAlpha, WgpuBlendOp::Add};
    state.alpha = {WgpuBlendFactor::One, WgpuBlendFactor::OneMinusSrcAlpha, WgpuBlendOp::Add};
    return state;
}

std::uint32_t padded_bytes_per_row(std::uint32_t width, rhi::TextureFormat format) {
    return static_cast<std::uint32_t>(
        align_up(static_cast<std::uint64_t>(width) * bytes_per_pixel(format), kCopyRowAlignment));
}

std::uint64_t readback_buffer_size(rhi::Extent2D extent, rhi::TextureFormat format) {
    return static_cast<std::uint64_t>(padded_bytes_per_row(extent.width, format)) * extent.height;
}

std::uint32_t push_constant_uniform_size(std::uint32_t push_constant_size) {
    return static_cast<std::uint32_t>(align_up(push_constant_size, kUniformAlignment));
}

std::uint32_t uniform_slice_stride(std::uint32_t push_constant_size) {
    return static_cast<std::uint32_t>(align_up(push_constant_size, kUniformSliceAlignment));
}

rhi::Scissor clamp_scissor(const rhi::Scissor& scissor, rhi::Extent2D target) {
    return rhi::intersect(scissor, rhi::Scissor{0, 0, target.width, target.height});
}

VertexLayoutError validate_vertex_layout(const rhi::VertexBufferLayout& layout) {
    if (layout.attribute_count == 0 || layout.attributes == nullptr) {
        return VertexLayoutError::MissingAttributes;
    }
    if (layout.attribute_count > kMaxVertexAttributes) {
        return VertexLayoutError::TooManyAttributes;
    }
    if (layout.stride % 4 != 0) {
        return VertexLayoutError::StrideUnaligned;
    }
    if (layout.stride > kMaxVertexStride) {
        return VertexLayoutError::StrideTooLarge;
    }
    std::uint32_t seen_locations = 0;
    for (std::uint32_t i = 0; i < layout.attribute_count; ++i) {
        const rhi::VertexAttribute& attribute = layout.attributes[i];
        if (attribute.location >= kMaxVertexAttributes) {
            return VertexLayoutError::LocationOutOfRange;
        }
        const std::uint32_t location_bit = 1u << attribute.location;
        if ((seen_locations & location_bit) != 0) {
            return VertexLayoutError::DuplicateLocation;
        }
        seen_locations |= location_bit;
        // Every supported format has 4-byte element alignment.
        if (attribute.offset % 4 != 0) {
            return VertexLayoutError::OffsetUnaligned;
        }
        if (attribute.offset + vertex_format_size(attribute.format) > layout.stride) {
            return VertexLayoutError::AttributeOutOfStride;
        }
    }
    return VertexLayoutError::None;
}

} // namespace tessera::rendering::webgpu
