// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

#include <cstdint>
#include <string_view>

namespace tessera::rendering::metal {

// RHI -> Metal descriptor mapping (ADR-053, docs/design/gpu/metal-backend.md).
//
// This header is deliberately free of <Metal/Metal.h> and of Objective-C: it is the half of the
// backend that builds and is unit-tested on every platform, exactly like the WebGPU mapping tables
// (src/rendering/backend/webgpu/src/webgpu_mapping.cpp). The device half includes this and casts
// the values to the real `MTL*` enumerators.
//
// The values below are the documented numeric values of the Metal enumerators, not guesses: they
// are pinned here so the mapping can be tested without a Metal toolchain, and a static_assert in
// the device half checks each one against the SDK's own enumerator on a Mac. That assert is what
// keeps this table honest; a mismatch is a compile error there, not a silent wrong format.

// MTLPixelFormat (Apple: MTLPixelFormat.h).
enum class PixelFormat : std::uint32_t {
    Invalid = 0,
    R8Unorm = 10,
    R8UnormSrgb = 11,
    RGBA8Unorm = 70,
    RGBA8UnormSrgb = 71,
    BGRA8Unorm = 80,
    BGRA8UnormSrgb = 81,
};

// MTLVertexFormat (Apple: MTLVertexFormat.h).
enum class VertexFormat : std::uint32_t {
    Invalid = 0,
    UChar4Normalized = 9,
    Float = 28,
    Float2 = 29,
    Float3 = 30,
    Float4 = 31,
};

// MTLResourceOptions storage modes (Apple: MTLResourceOptions.h). `Shared` is 0, so a
// zero-initialized option is the CPU-visible mode, which is the safe default for a staging buffer.
enum class StorageMode : std::uint32_t {
    Shared = 0,
    Managed = 16,
    Private = 32,
};

// MTLResourceOptions hazard tracking (Apple: MTLResourceOptions.h). ADR-053 decision 3 keeps
// default (tracked) hazard tracking and forbids untracked resources until measurements justify
// them, so the backend only ever asks for `Tracked`.
enum class HazardTracking : std::uint32_t {
    Untracked = 256,
    Tracked = 512,
};

// MTLLoadAction / MTLStoreAction (Apple: MTLLoadAction.h, MTLStoreAction.h).
enum class LoadAction : std::uint32_t {
    DontCare = 0,
    Load = 1,
    Clear = 2,
};

enum class StoreAction : std::uint32_t {
    DontCare = 0,
    Store = 1,
    MultisampleResolve = 2,
    StoreAndMultisampleResolve = 3,
};

// MTLSamplerMinMagFilter / MTLSamplerAddressMode / MTLSamplerBorderColor.
enum class SamplerFilter : std::uint32_t {
    Nearest = 0,
    Linear = 1,
};

enum class SamplerAddressMode : std::uint32_t {
    ClampToEdge = 0,
    MirrorClampToEdge = 1,
    Repeat = 2,
    MirrorRepeat = 3,
    ClampToZero = 4,
    ClampToBorderColor = 5,
};

enum class SamplerBorderColor : std::uint32_t {
    TransparentBlack = 0,
    OpaqueBlack = 1,
    OpaqueWhite = 2,
};

// MTLBlendFactor / MTLBlendOperation.
enum class BlendFactor : std::uint32_t {
    Zero = 0,
    One = 1,
    SourceColor = 2,
    OneMinusSourceColor = 3,
    SourceAlpha = 4,
    OneMinusSourceAlpha = 5,
    DestinationColor = 6,
    OneMinusDestinationColor = 7,
    DestinationAlpha = 8,
    OneMinusDestinationAlpha = 9,
    SourceAlphaSaturated = 10,
    BlendColor = 11,
    OneMinusBlendColor = 12,
    BlendAlpha = 13,
    OneMinusBlendAlpha = 14,
    Source1Color = 15,
    OneMinusSource1Color = 16,
    Source1Alpha = 17,
    OneMinusSource1Alpha = 18,
};

enum class BlendOperation : std::uint32_t {
    Add = 0,
    Subtract = 1,
    ReverseSubtract = 2,
    Min = 3,
    Max = 4,
};

// The colour-attachment blend state one RHI `BlendMode` maps to. `enabled == false` means the
// attachment writes the fragment output unchanged (RHI `BlendMode::None`).
struct BlendState {
    bool enabled = false;
    BlendFactor source_color = BlendFactor::One;
    BlendFactor destination_color = BlendFactor::Zero;
    BlendOperation color_operation = BlendOperation::Add;
    BlendFactor source_alpha = BlendFactor::One;
    BlendFactor destination_alpha = BlendFactor::Zero;
    BlendOperation alpha_operation = BlendOperation::Add;
};

// --- mapping ---------------------------------------------------------------------------------

// Throws std::invalid_argument for a format Metal cannot express as a colour attachment. Every RHI
// format is 8-bit colour, so this is total today; it stays a checked function so a future format
// cannot silently become `Invalid`.
[[nodiscard]] PixelFormat pixel_format(rhi::TextureFormat format);

// Throws std::invalid_argument for a vertex format Metal cannot express.
[[nodiscard]] VertexFormat vertex_format(rhi::VertexFormat format);

// Bytes per pixel of an RHI texture format (1 for R8Unorm, 4 otherwise).
[[nodiscard]] std::uint32_t bytes_per_pixel(rhi::TextureFormat format);

// The storage mode a resource of this use is created with (docs/design/gpu/metal-backend.md
// section 4.1): CPU-written buffers are `Shared`, GPU-only textures and render targets are
// `Private`. `Managed` is never chosen: it needs explicit `didModifyRange:`/`synchronizeResource:`
// and exists only for discrete GPUs.
[[nodiscard]] StorageMode buffer_storage_mode(rhi::BufferUsage usage);
[[nodiscard]] StorageMode texture_storage_mode();

// True when the buffer's contents are written by the CPU and read by the GPU through a mapping
// (`Shared`), which is what `Buffer::write`/`read` require.
[[nodiscard]] bool buffer_is_cpu_visible(rhi::BufferUsage usage);

[[nodiscard]] LoadAction load_action(rhi::LoadOp op);
[[nodiscard]] StoreAction store_action(rhi::StoreOp op);

[[nodiscard]] SamplerFilter sampler_filter(rhi::Filter filter);
[[nodiscard]] SamplerAddressMode sampler_address_mode(rhi::AddressMode mode);

// The blend state for an RHI blend mode. `DualSource` maps to the SRC1 factors exactly as the
// Vulkan backend does (`src0 * src1 + dst * (1 - src1)` per channel); whether the device can
// express it is `Device::supports_dual_source_blend()`, and a device that cannot must reject the
// pipeline instead of calling this (RHI 1.5, docs/design/gpu/rhi.md section 4.1).
[[nodiscard]] BlendState blend_state(rhi::BlendMode mode);

// True when the RHI blend mode needs the second fragment output (dual-source blending).
[[nodiscard]] constexpr bool needs_dual_source(rhi::BlendMode mode) {
    return mode == rhi::BlendMode::DualSource;
}

// A stable name for logging and conformance output.
[[nodiscard]] std::string_view backend_name();

} // namespace tessera::rendering::metal
