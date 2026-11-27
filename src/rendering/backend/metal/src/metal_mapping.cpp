// SPDX-License-Identifier: MIT

#include "tessera/rendering/metal/metal_mapping.hpp"

#include <stdexcept>
#include <string>

namespace tessera::rendering::metal {
namespace {

[[nodiscard]] const char* format_name(rhi::TextureFormat format) {
    switch (format) {
    case rhi::TextureFormat::R8G8B8A8Unorm:
        return "R8G8B8A8Unorm";
    case rhi::TextureFormat::R8Unorm:
        return "R8Unorm";
    case rhi::TextureFormat::B8G8R8A8Unorm:
        return "B8G8R8A8Unorm";
    case rhi::TextureFormat::R8G8B8A8Srgb:
        return "R8G8B8A8Srgb";
    case rhi::TextureFormat::B8G8R8A8Srgb:
        return "B8G8R8A8Srgb";
    }
    return "unknown";
}

} // namespace

PixelFormat pixel_format(rhi::TextureFormat format) {
    switch (format) {
    case rhi::TextureFormat::R8G8B8A8Unorm:
        return PixelFormat::RGBA8Unorm;
    case rhi::TextureFormat::R8Unorm:
        return PixelFormat::R8Unorm;
    case rhi::TextureFormat::B8G8R8A8Unorm:
        return PixelFormat::BGRA8Unorm;
    case rhi::TextureFormat::R8G8B8A8Srgb:
        return PixelFormat::RGBA8UnormSrgb;
    case rhi::TextureFormat::B8G8R8A8Srgb:
        return PixelFormat::BGRA8UnormSrgb;
    }
    throw std::invalid_argument(std::string("metal: unsupported texture format ") +
                                format_name(format));
}

VertexFormat vertex_format(rhi::VertexFormat format) {
    switch (format) {
    case rhi::VertexFormat::Float:
        return VertexFormat::Float;
    case rhi::VertexFormat::Float2:
        return VertexFormat::Float2;
    case rhi::VertexFormat::Float3:
        return VertexFormat::Float3;
    case rhi::VertexFormat::Float4:
        return VertexFormat::Float4;
    case rhi::VertexFormat::UByte4Norm:
        return VertexFormat::UChar4Normalized;
    case rhi::VertexFormat::UInt:
        return VertexFormat::UInt;
    }
    throw std::invalid_argument("metal: unsupported vertex format");
}

std::uint32_t bytes_per_pixel(rhi::TextureFormat format) {
    return format == rhi::TextureFormat::R8Unorm ? 1U : 4U;
}

StorageMode buffer_storage_mode(rhi::BufferUsage usage) {
    switch (usage) {
    case rhi::BufferUsage::Vertex:
    case rhi::BufferUsage::Index:
    case rhi::BufferUsage::Uniform:
    case rhi::BufferUsage::Staging:
    case rhi::BufferUsage::Readback:
    // A read-only storage buffer (RHI 1.12, ADR-068) is written by the CPU every frame, so it needs
    // the same CPU-visible mode as the upload buffers.
    case rhi::BufferUsage::Storage:
        // Every RHI buffer use is written or read by the CPU: vertex/index/uniform data is
        // uploaded, staging is written, readback is read. `Shared` is the mode that allows both
        // without the explicit synchronize calls `Managed` needs (section 4.1).
        return StorageMode::Shared;
    }
    return StorageMode::Shared;
}

StorageMode texture_storage_mode() {
    // Textures and render targets are GPU-only; uploads go through a staging buffer and a blit
    // encoder, so the texture itself never needs CPU access (section 4.1).
    return StorageMode::Private;
}

bool buffer_is_cpu_visible(rhi::BufferUsage usage) {
    return buffer_storage_mode(usage) == StorageMode::Shared;
}

LoadAction load_action(rhi::LoadOp op) {
    switch (op) {
    case rhi::LoadOp::Clear:
        return LoadAction::Clear;
    case rhi::LoadOp::Load:
        return LoadAction::Load;
    case rhi::LoadOp::DontCare:
        return LoadAction::DontCare;
    }
    return LoadAction::Clear;
}

StoreAction store_action(rhi::StoreOp op) {
    switch (op) {
    case rhi::StoreOp::Store:
        return StoreAction::Store;
    case rhi::StoreOp::DontCare:
        return StoreAction::DontCare;
    }
    return StoreAction::Store;
}

SamplerFilter sampler_filter(rhi::Filter filter) {
    return filter == rhi::Filter::Linear ? SamplerFilter::Linear : SamplerFilter::Nearest;
}

SamplerAddressMode sampler_address_mode(rhi::AddressMode mode) {
    return mode == rhi::AddressMode::Repeat ? SamplerAddressMode::Repeat
                                            : SamplerAddressMode::ClampToEdge;
}

BlendState blend_state(rhi::BlendMode mode) {
    BlendState state;
    switch (mode) {
    case rhi::BlendMode::None:
        return state;
    case rhi::BlendMode::Alpha:
        // Straight alpha, matching the Vulkan backend exactly: colour is `src * srcA + dst *
        // (1 - srcA)` and alpha is `src * 1 + dst * (1 - srcA)`.
        state.enabled = true;
        state.source_color = BlendFactor::SourceAlpha;
        state.destination_color = BlendFactor::OneMinusSourceAlpha;
        state.color_operation = BlendOperation::Add;
        state.source_alpha = BlendFactor::One;
        state.destination_alpha = BlendFactor::OneMinusSourceAlpha;
        state.alpha_operation = BlendOperation::Add;
        return state;
    case rhi::BlendMode::DestinationIn:
        // Mask step (RHI 1.19): `dst * src.a` on all four channels, as the Vulkan backend records
        // it.
        state.enabled = true;
        state.source_color = BlendFactor::Zero;
        state.destination_color = BlendFactor::SourceAlpha;
        state.color_operation = BlendOperation::Add;
        state.source_alpha = BlendFactor::Zero;
        state.destination_alpha = BlendFactor::SourceAlpha;
        state.alpha_operation = BlendOperation::Add;
        return state;
    case rhi::BlendMode::SourceOut:
    case rhi::BlendMode::Xor:
        // Mask combine (RHI 1.21), the same factors as the Vulkan backend.
        state.enabled = true;
        state.source_color = BlendFactor::OneMinusDestinationAlpha;
        state.destination_color =
            mode == rhi::BlendMode::Xor ? BlendFactor::OneMinusSourceAlpha : BlendFactor::Zero;
        state.color_operation = BlendOperation::Add;
        state.source_alpha = BlendFactor::OneMinusDestinationAlpha;
        state.destination_alpha = state.destination_color;
        state.alpha_operation = BlendOperation::Add;
        return state;
    case rhi::BlendMode::DualSource:
        // Per-channel coverage (RHI 1.5): `src0 * src1 + dst * (1 - src1)`, with the SRC1 colour
        // factors used for alpha too so one coverage value drives all four channels, exactly as
        // the Vulkan backend records it.
        state.enabled = true;
        state.source_color = BlendFactor::Source1Color;
        state.destination_color = BlendFactor::OneMinusSource1Color;
        state.color_operation = BlendOperation::Add;
        state.source_alpha = BlendFactor::Source1Color;
        state.destination_alpha = BlendFactor::OneMinusSource1Color;
        state.alpha_operation = BlendOperation::Add;
        return state;
    }
    return state;
}

std::string_view backend_name() {
    return "metal";
}

} // namespace tessera::rendering::metal
