// SPDX-License-Identifier: MIT

// The RHI -> Metal mapping tables (ADR-053). API-free by design, so these run on every host: they
// pin the numeric Metal enumerator values the device half casts to, and the blend state that must
// match the Vulkan backend exactly.

#include "tessera/rendering/metal/metal_mapping.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

using namespace tessera::rendering;
using namespace tessera::rendering::metal;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "metal_mapping: " << message << "\n";
    }
    return condition;
}

bool check_pixel_formats() {
    // The numeric values are Metal's documented enumerator values; the device half static_asserts
    // them against the SDK, so a wrong number here is caught on a Mac and a wrong mapping is caught
    // here on every host.
    return expect(pixel_format(rhi::TextureFormat::R8G8B8A8Unorm) == PixelFormat::RGBA8Unorm,
                  "R8G8B8A8Unorm -> RGBA8Unorm") &&
           expect(static_cast<std::uint32_t>(PixelFormat::RGBA8Unorm) == 70, "RGBA8Unorm is 70") &&
           expect(pixel_format(rhi::TextureFormat::R8Unorm) == PixelFormat::R8Unorm,
                  "R8Unorm -> R8Unorm") &&
           expect(static_cast<std::uint32_t>(PixelFormat::R8Unorm) == 10, "R8Unorm is 10") &&
           expect(pixel_format(rhi::TextureFormat::B8G8R8A8Unorm) == PixelFormat::BGRA8Unorm,
                  "B8G8R8A8Unorm -> BGRA8Unorm") &&
           expect(static_cast<std::uint32_t>(PixelFormat::BGRA8Unorm) == 80, "BGRA8Unorm is 80") &&
           expect(pixel_format(rhi::TextureFormat::R8G8B8A8Srgb) == PixelFormat::RGBA8UnormSrgb,
                  "R8G8B8A8Srgb -> RGBA8UnormSrgb") &&
           expect(static_cast<std::uint32_t>(PixelFormat::RGBA8UnormSrgb) == 71,
                  "RGBA8UnormSrgb is 71") &&
           expect(pixel_format(rhi::TextureFormat::B8G8R8A8Srgb) == PixelFormat::BGRA8UnormSrgb,
                  "B8G8R8A8Srgb -> BGRA8UnormSrgb") &&
           expect(static_cast<std::uint32_t>(PixelFormat::BGRA8UnormSrgb) == 81,
                  "BGRA8UnormSrgb is 81");
}

bool check_vertex_formats() {
    return expect(vertex_format(rhi::VertexFormat::Float) == VertexFormat::Float,
                  "Float -> Float") &&
           expect(static_cast<std::uint32_t>(VertexFormat::Float) == 28, "Float is 28") &&
           expect(vertex_format(rhi::VertexFormat::Float2) == VertexFormat::Float2,
                  "Float2 -> Float2") &&
           expect(static_cast<std::uint32_t>(VertexFormat::Float2) == 29, "Float2 is 29") &&
           expect(vertex_format(rhi::VertexFormat::Float3) == VertexFormat::Float3,
                  "Float3 -> Float3") &&
           expect(static_cast<std::uint32_t>(VertexFormat::Float3) == 30, "Float3 is 30") &&
           expect(vertex_format(rhi::VertexFormat::Float4) == VertexFormat::Float4,
                  "Float4 -> Float4") &&
           expect(static_cast<std::uint32_t>(VertexFormat::Float4) == 31, "Float4 is 31") &&
           expect(vertex_format(rhi::VertexFormat::UByte4Norm) == VertexFormat::UChar4Normalized,
                  "UByte4Norm -> UChar4Normalized") &&
           expect(static_cast<std::uint32_t>(VertexFormat::UChar4Normalized) == 9,
                  "UChar4Normalized is 9");
}

bool check_bytes_per_pixel() {
    return expect(bytes_per_pixel(rhi::TextureFormat::R8Unorm) == 1, "R8Unorm is 1 byte") &&
           expect(bytes_per_pixel(rhi::TextureFormat::R8G8B8A8Unorm) == 4, "RGBA8 is 4 bytes") &&
           expect(bytes_per_pixel(rhi::TextureFormat::B8G8R8A8Unorm) == 4, "BGRA8 is 4 bytes") &&
           expect(bytes_per_pixel(rhi::TextureFormat::R8G8B8A8Srgb) == 4,
                  "RGBA8 sRGB is 4 bytes") &&
           expect(bytes_per_pixel(rhi::TextureFormat::B8G8R8A8Srgb) == 4, "BGRA8 sRGB is 4 bytes");
}

bool check_storage_modes() {
    // Section 4.1: CPU-written buffers are Shared, GPU-only textures are Private, and Managed is
    // never chosen (it needs explicit synchronize calls and exists only for discrete GPUs).
    const rhi::BufferUsage uses[] = {rhi::BufferUsage::Vertex, rhi::BufferUsage::Index,
                                     rhi::BufferUsage::Uniform, rhi::BufferUsage::Staging,
                                     rhi::BufferUsage::Readback};
    for (const rhi::BufferUsage use : uses) {
        if (!expect(buffer_storage_mode(use) == StorageMode::Shared, "buffers are Shared") ||
            !expect(buffer_is_cpu_visible(use), "buffers are CPU visible")) {
            return false;
        }
    }
    return expect(texture_storage_mode() == StorageMode::Private, "textures are Private") &&
           expect(static_cast<std::uint32_t>(StorageMode::Shared) == 0, "Shared is 0") &&
           expect(static_cast<std::uint32_t>(StorageMode::Managed) == 16, "Managed is 16") &&
           expect(static_cast<std::uint32_t>(StorageMode::Private) == 32, "Private is 32") &&
           expect(static_cast<std::uint32_t>(HazardTracking::Tracked) == 512, "Tracked is 512");
}

bool check_load_store_actions() {
    return expect(load_action(rhi::LoadOp::Clear) == LoadAction::Clear, "Clear -> Clear") &&
           expect(static_cast<std::uint32_t>(LoadAction::Clear) == 2, "Clear is 2") &&
           expect(load_action(rhi::LoadOp::Load) == LoadAction::Load, "Load -> Load") &&
           expect(static_cast<std::uint32_t>(LoadAction::Load) == 1, "Load is 1") &&
           expect(load_action(rhi::LoadOp::DontCare) == LoadAction::DontCare,
                  "DontCare -> DontCare") &&
           expect(static_cast<std::uint32_t>(LoadAction::DontCare) == 0, "DontCare is 0") &&
           expect(store_action(rhi::StoreOp::Store) == StoreAction::Store, "Store -> Store") &&
           expect(static_cast<std::uint32_t>(StoreAction::Store) == 1, "Store is 1") &&
           expect(store_action(rhi::StoreOp::DontCare) == StoreAction::DontCare,
                  "DontCare -> DontCare") &&
           expect(static_cast<std::uint32_t>(StoreAction::DontCare) == 0, "DontCare is 0");
}

bool check_sampler_state() {
    return expect(sampler_filter(rhi::Filter::Nearest) == SamplerFilter::Nearest,
                  "Nearest -> Nearest") &&
           expect(static_cast<std::uint32_t>(SamplerFilter::Nearest) == 0, "Nearest is 0") &&
           expect(sampler_filter(rhi::Filter::Linear) == SamplerFilter::Linear,
                  "Linear -> Linear") &&
           expect(static_cast<std::uint32_t>(SamplerFilter::Linear) == 1, "Linear is 1") &&
           expect(sampler_address_mode(rhi::AddressMode::ClampToEdge) ==
                      SamplerAddressMode::ClampToEdge,
                  "ClampToEdge -> ClampToEdge") &&
           expect(static_cast<std::uint32_t>(SamplerAddressMode::ClampToEdge) == 0,
                  "ClampToEdge is 0") &&
           expect(sampler_address_mode(rhi::AddressMode::Repeat) == SamplerAddressMode::Repeat,
                  "Repeat -> Repeat") &&
           expect(static_cast<std::uint32_t>(SamplerAddressMode::Repeat) == 2, "Repeat is 2");
}

bool check_blend_none() {
    const BlendState state = blend_state(rhi::BlendMode::None);
    return expect(!state.enabled, "None disables blending");
}

bool check_blend_alpha_matches_vulkan() {
    // Vulkan records: colour srcA / 1-srcA, alpha 1 / 1-srcA, both ADD. Metal must be identical or
    // the two backends composite differently.
    const BlendState state = blend_state(rhi::BlendMode::Alpha);
    return expect(state.enabled, "Alpha enables blending") &&
           expect(state.source_color == BlendFactor::SourceAlpha, "colour source is srcA") &&
           expect(state.destination_color == BlendFactor::OneMinusSourceAlpha,
                  "colour destination is 1-srcA") &&
           expect(state.color_operation == BlendOperation::Add, "colour op is Add") &&
           expect(state.source_alpha == BlendFactor::One, "alpha source is One") &&
           expect(state.destination_alpha == BlendFactor::OneMinusSourceAlpha,
                  "alpha destination is 1-srcA") &&
           expect(state.alpha_operation == BlendOperation::Add, "alpha op is Add") &&
           expect(static_cast<std::uint32_t>(BlendFactor::SourceAlpha) == 4, "SourceAlpha is 4") &&
           expect(static_cast<std::uint32_t>(BlendFactor::OneMinusSourceAlpha) == 5,
                  "OneMinusSourceAlpha is 5");
}

bool check_blend_dual_source_matches_vulkan() {
    // Vulkan records SRC1_COLOR / ONE_MINUS_SRC1_COLOR for both colour and alpha.
    const BlendState state = blend_state(rhi::BlendMode::DualSource);
    return expect(state.enabled, "DualSource enables blending") &&
           expect(state.source_color == BlendFactor::Source1Color, "colour source is src1") &&
           expect(state.destination_color == BlendFactor::OneMinusSource1Color,
                  "colour destination is 1-src1") &&
           expect(state.source_alpha == BlendFactor::Source1Color, "alpha source is src1") &&
           expect(state.destination_alpha == BlendFactor::OneMinusSource1Color,
                  "alpha destination is 1-src1") &&
           expect(static_cast<std::uint32_t>(BlendFactor::Source1Color) == 15,
                  "Source1Color is 15") &&
           expect(static_cast<std::uint32_t>(BlendFactor::OneMinusSource1Color) == 16,
                  "OneMinusSource1Color is 16") &&
           expect(needs_dual_source(rhi::BlendMode::DualSource), "DualSource needs src1") &&
           expect(!needs_dual_source(rhi::BlendMode::Alpha), "Alpha does not need src1") &&
           expect(!needs_dual_source(rhi::BlendMode::None), "None does not need src1");
}

bool check_backend_name() {
    return expect(backend_name() == std::string_view("metal"), "backend name is metal");
}

} // namespace

int main() {
    if (!check_pixel_formats() || !check_vertex_formats() || !check_bytes_per_pixel() ||
        !check_storage_modes() || !check_load_store_actions() || !check_sampler_state() ||
        !check_blend_none() || !check_blend_alpha_matches_vulkan() ||
        !check_blend_dual_source_matches_vulkan() || !check_backend_name()) {
        return 1;
    }
    return 0;
}
