// SPDX-License-Identifier: MIT

#include "tessera/rendering/webgpu/webgpu_mapping.hpp"

#include <stdexcept>

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
    case rhi::BufferUsage::Storage:
        // RHI 1.12 (ADR-068): a read-only storage buffer the fragment shader indexes. COPY_DST
        // because the CPU writes the record list through a staging copy, STORAGE because the shader
        // reads it. The binding itself lands with ADR-068 slice 2; the usage mapping is here so a
        // buffer created with it is well formed on this backend too.
        return bits::kStorage | bits::kCopyDst;
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
    // COPY_DST as well as COPY_SRC: the isolated-layer seam copies a region of one offscreen target
    // into another (CommandList::copy_target_region, issue #242), so a 1-sample render target is a
    // legal copy destination, and TEXTURE_BINDING lets a later pass sample the copy.
    return bits::kRenderAttachment | bits::kTextureBinding | bits::kCopySrc | bits::kCopyDst;
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
    case rhi::VertexFormat::UInt:
        // The per-vertex texture-array index (RHI 1.7, issue #480 part 2). WebGPU has no descriptor
        // indexing, so the WGSL twin ignores the value; the attribute still maps so one Vertex2D
        // serves both backends.
        return WgpuVertexFormat::Uint32;
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
    case rhi::VertexFormat::UInt:
        return 4;
    }
    return 0;
}

// Same factors as the Vulkan backend, for the modes WebGPU can express. `DualSource` is rejected
// rather than mapped: WebGPU has one fragment output per target and no SRC1 blend factor, so the
// per-channel coverage of LCD text cannot be expressed at all. Returning the disabled state for it
// (which is what this function used to do for every non-Alpha mode) would write the raw coverage
// channel instead of the documented grayscale fallback, so the request throws instead (issue #471).
WgpuBlendState blend_state(rhi::BlendMode mode) {
    WgpuBlendState state;
    if (mode == rhi::BlendMode::DualSource) {
        throw std::invalid_argument("blend_state: dual-source blending is not supported by WebGPU; "
                                    "the caller must fall back to grayscale coverage");
    }
    if (mode == rhi::BlendMode::DestinationIn) {
        // Mask step (RHI 1.19), the same factors as the Vulkan backend: `dst * src.a` on all four
        // channels.
        state.enabled = true;
        state.color = {WgpuBlendFactor::Zero, WgpuBlendFactor::SrcAlpha, WgpuBlendOp::Add};
        state.alpha = {WgpuBlendFactor::Zero, WgpuBlendFactor::SrcAlpha, WgpuBlendOp::Add};
        return state;
    }
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
