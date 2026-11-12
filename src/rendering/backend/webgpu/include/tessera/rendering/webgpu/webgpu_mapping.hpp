// SPDX-License-Identifier: MIT

#pragma once

// RHI descriptor -> WebGPU descriptor mapping tables.
//
// Pure functions over plain enums so they build and test on every host: this header includes no
// WebGPU header. The Emscripten-only device code converts these enums to the matching `WGPU*`
// values with a switch (enum numbering differs between webgpu.h revisions, so no cast is relied
// on), except the usage bit masks, whose values are fixed by the WebGPU specification.

#include "tessera/rendering/rhi/types.hpp"

#include <cstdint>
#include <string_view>

namespace tessera::rendering::webgpu {

enum class WgpuTextureFormat { Rgba8Unorm, R8Unorm, Bgra8Unorm, Rgba8UnormSrgb, Bgra8UnormSrgb };
enum class WgpuFilterMode { Nearest, Linear };
enum class WgpuAddressMode { ClampToEdge, Repeat };
// WebGPU has no "don't care" load op; see load_op().
enum class WgpuLoadOp { Clear, Load };
enum class WgpuStoreOp { Store, Discard };
enum class WgpuVertexFormat { Float32, Float32x2, Float32x3, Float32x4, Unorm8x4 };
enum class WgpuBlendFactor { One, SrcAlpha, OneMinusSrcAlpha };
enum class WgpuBlendOp { Add };
enum class WgpuPrimitiveTopology { TriangleList };

// Values are the WebGPU specification's GPUBufferUsage / GPUTextureUsage bits.
namespace buffer_usage_bits {
inline constexpr std::uint32_t kMapRead = 0x01;
inline constexpr std::uint32_t kMapWrite = 0x02;
inline constexpr std::uint32_t kCopySrc = 0x04;
inline constexpr std::uint32_t kCopyDst = 0x08;
inline constexpr std::uint32_t kIndex = 0x10;
inline constexpr std::uint32_t kVertex = 0x20;
inline constexpr std::uint32_t kUniform = 0x40;
} // namespace buffer_usage_bits

namespace texture_usage_bits {
inline constexpr std::uint32_t kCopySrc = 0x01;
inline constexpr std::uint32_t kCopyDst = 0x02;
inline constexpr std::uint32_t kTextureBinding = 0x04;
inline constexpr std::uint32_t kRenderAttachment = 0x10;
} // namespace texture_usage_bits

struct WgpuBlendComponent {
    WgpuBlendFactor src = WgpuBlendFactor::One;
    WgpuBlendFactor dst = WgpuBlendFactor::One;
    WgpuBlendOp op = WgpuBlendOp::Add;
};

struct WgpuBlendState {
    // False means the color target carries no blend state at all (opaque write).
    bool enabled = false;
    WgpuBlendComponent color;
    WgpuBlendComponent alpha;
};

[[nodiscard]] WgpuTextureFormat texture_format(rhi::TextureFormat format);
[[nodiscard]] std::string_view texture_format_name(WgpuTextureFormat format);
[[nodiscard]] std::uint32_t bytes_per_pixel(rhi::TextureFormat format);

[[nodiscard]] std::uint32_t buffer_usage_flags(rhi::BufferUsage usage);
// Sampled texture that is uploaded to (atlas, image).
[[nodiscard]] std::uint32_t sampled_texture_usage();
// 1-sample targets are rendered to, sampled, and copied out for readback. A 4x target is only
// rendered to; its resolve image carries the 1-sample usage.
[[nodiscard]] std::uint32_t render_target_usage(std::uint32_t sample_count);

[[nodiscard]] WgpuFilterMode filter_mode(rhi::Filter filter);
[[nodiscard]] WgpuAddressMode address_mode(rhi::AddressMode mode);
[[nodiscard]] WgpuLoadOp load_op(rhi::LoadOp op);
[[nodiscard]] WgpuStoreOp store_op(rhi::StoreOp op);
[[nodiscard]] WgpuVertexFormat vertex_format(rhi::VertexFormat format);
[[nodiscard]] std::uint32_t vertex_format_size(rhi::VertexFormat format);
[[nodiscard]] WgpuBlendState blend_state(rhi::BlendMode mode);
[[nodiscard]] constexpr WgpuPrimitiveTopology primitive_topology() {
    return WgpuPrimitiveTopology::TriangleList;
}

// WebGPU supports exactly 1 and 4 samples, the same set the RHI accepts.
[[nodiscard]] constexpr bool is_supported_sample_count(std::uint32_t sample_count) {
    return rhi::is_supported_sample_count(sample_count);
}

[[nodiscard]] constexpr std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

// copyTextureToBuffer requires bytesPerRow to be a multiple of 256; writeTexture does not.
inline constexpr std::uint32_t kCopyRowAlignment = 256;
[[nodiscard]] std::uint32_t padded_bytes_per_row(std::uint32_t width, rhi::TextureFormat format);
// Size of a staging buffer that receives a full-target copy with padded rows.
[[nodiscard]] std::uint64_t readback_buffer_size(rhi::Extent2D extent, rhi::TextureFormat format);

// WebGPU has no push constants; the backend emulates them with a uniform buffer in group 0,
// binding 0. Uniform bindings and buffer writes want 16-byte (and at least 4-byte) multiples.
inline constexpr std::uint32_t kUniformAlignment = 16;
[[nodiscard]] std::uint32_t push_constant_uniform_size(std::uint32_t push_constant_size);

// Every draw may see different push-constant values, but queue writes land before the whole command
// buffer runs, so each distinct block gets its own slice of a per-command-list uniform arena bound
// with a dynamic offset. Dynamic offsets must be multiples of minUniformBufferOffsetAlignment
// (256 at the default limits).
inline constexpr std::uint32_t kUniformSliceAlignment = 256;
[[nodiscard]] std::uint32_t uniform_slice_stride(std::uint32_t push_constant_size);

// setScissorRect takes unsigned values inside the attachment; the RHI scissor may start negative or
// overhang. Returns an empty (zero-area) scissor when nothing remains.
[[nodiscard]] rhi::Scissor clamp_scissor(const rhi::Scissor& scissor, rhi::Extent2D target);

// Bind group 0 layout shared by every RHI pipeline (see docs/design/gpu/webgpu-backend.md).
inline constexpr std::uint32_t kBindingPushConstants = 0;
inline constexpr std::uint32_t kBindingTexture = 1;
inline constexpr std::uint32_t kBindingSampler = 2;

// Default-limit validation for a vertex buffer layout.
enum class VertexLayoutError {
    None,
    TooManyAttributes,
    StrideUnaligned,
    StrideTooLarge,
    LocationOutOfRange,
    DuplicateLocation,
    OffsetUnaligned,
    AttributeOutOfStride,
    MissingAttributes,
};
inline constexpr std::uint32_t kMaxVertexAttributes = 16;
inline constexpr std::uint32_t kMaxVertexStride = 2048;
[[nodiscard]] VertexLayoutError validate_vertex_layout(const rhi::VertexBufferLayout& layout);

} // namespace tessera::rendering::webgpu
