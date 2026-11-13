// SPDX-License-Identifier: MIT

#include "tessera/rendering/gpu/shader_ids.hpp"
#include "tessera/rendering/gpu/vertex2d.hpp"
#include "tessera/rendering/webgpu/shader_registry.hpp"
#include "tessera/rendering/webgpu/webgpu_mapping.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace rhi = tessera::rendering::rhi;
namespace wg = tessera::rendering::webgpu;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "webgpu_mapping: " << message << "\n";
        ++g_failures;
    }
}

void test_texture_formats() {
    const struct {
        rhi::TextureFormat rhi_format;
        wg::WgpuTextureFormat wgpu_format;
        std::string_view name;
        std::uint32_t bytes;
    } cases[] = {
        {rhi::TextureFormat::R8G8B8A8Unorm, wg::WgpuTextureFormat::Rgba8Unorm, "rgba8unorm", 4},
        {rhi::TextureFormat::R8Unorm, wg::WgpuTextureFormat::R8Unorm, "r8unorm", 1},
        {rhi::TextureFormat::B8G8R8A8Unorm, wg::WgpuTextureFormat::Bgra8Unorm, "bgra8unorm", 4},
        {rhi::TextureFormat::R8G8B8A8Srgb, wg::WgpuTextureFormat::Rgba8UnormSrgb, "rgba8unorm-srgb",
         4},
        {rhi::TextureFormat::B8G8R8A8Srgb, wg::WgpuTextureFormat::Bgra8UnormSrgb, "bgra8unorm-srgb",
         4},
    };
    for (const auto& c : cases) {
        expect(wg::texture_format(c.rhi_format) == c.wgpu_format, "texture format mapping");
        expect(wg::texture_format_name(c.wgpu_format) == c.name, "texture format name");
        expect(wg::bytes_per_pixel(c.rhi_format) == c.bytes, "bytes per pixel");
    }
}

void test_usage_flags() {
    namespace b = wg::buffer_usage_bits;
    namespace t = wg::texture_usage_bits;
    expect(wg::buffer_usage_flags(rhi::BufferUsage::Vertex) == (b::kVertex | b::kCopyDst),
           "vertex buffer usage");
    expect(wg::buffer_usage_flags(rhi::BufferUsage::Index) == (b::kIndex | b::kCopyDst),
           "index buffer usage");
    expect(wg::buffer_usage_flags(rhi::BufferUsage::Uniform) == (b::kUniform | b::kCopyDst),
           "uniform buffer usage");
    expect(wg::buffer_usage_flags(rhi::BufferUsage::Staging) == (b::kMapWrite | b::kCopySrc),
           "staging buffer usage");
    expect(wg::buffer_usage_flags(rhi::BufferUsage::Readback) == (b::kMapRead | b::kCopyDst),
           "readback buffer usage");
    // Spec-fixed bit values (the device code casts these straight into WGPU flag types).
    expect(b::kMapRead == 1 && b::kMapWrite == 2 && b::kCopySrc == 4 && b::kCopyDst == 8 &&
               b::kIndex == 16 && b::kVertex == 32 && b::kUniform == 64,
           "buffer usage bit values");
    expect(t::kCopySrc == 1 && t::kCopyDst == 2 && t::kTextureBinding == 4 &&
               t::kRenderAttachment == 16,
           "texture usage bit values");
    expect(wg::sampled_texture_usage() == (t::kTextureBinding | t::kCopyDst), "sampled usage");
    // 1x targets are readback sources AND copy_target_region destinations (the isolated-layer seam,
    // issue #242), so they carry both copy usages plus texture binding for the composite draw.
    expect(wg::render_target_usage(1) ==
               (t::kRenderAttachment | t::kTextureBinding | t::kCopySrc | t::kCopyDst),
           "1x target usage");
    expect(wg::render_target_usage(4) == t::kRenderAttachment, "4x target usage");
    // WebGPU forbids mixing MAP_READ/MAP_WRITE with anything but the matching copy usage.
    expect((wg::buffer_usage_flags(rhi::BufferUsage::Staging) & ~(b::kMapWrite | b::kCopySrc)) == 0,
           "staging usage obeys the map-write rule");
    expect((wg::buffer_usage_flags(rhi::BufferUsage::Readback) & ~(b::kMapRead | b::kCopyDst)) == 0,
           "readback usage obeys the map-read rule");
}

void test_sampler_and_ops() {
    expect(wg::filter_mode(rhi::Filter::Nearest) == wg::WgpuFilterMode::Nearest, "nearest");
    expect(wg::filter_mode(rhi::Filter::Linear) == wg::WgpuFilterMode::Linear, "linear");
    expect(wg::address_mode(rhi::AddressMode::ClampToEdge) == wg::WgpuAddressMode::ClampToEdge,
           "clamp");
    expect(wg::address_mode(rhi::AddressMode::Repeat) == wg::WgpuAddressMode::Repeat, "repeat");
    expect(wg::load_op(rhi::LoadOp::Clear) == wg::WgpuLoadOp::Clear, "load clear");
    expect(wg::load_op(rhi::LoadOp::Load) == wg::WgpuLoadOp::Load, "load load");
    expect(wg::load_op(rhi::LoadOp::DontCare) == wg::WgpuLoadOp::Clear, "load dont-care -> clear");
    expect(wg::store_op(rhi::StoreOp::Store) == wg::WgpuStoreOp::Store, "store store");
    expect(wg::store_op(rhi::StoreOp::DontCare) == wg::WgpuStoreOp::Discard, "store discard");
    expect(wg::primitive_topology() == wg::WgpuPrimitiveTopology::TriangleList, "topology");
}

void test_vertex_formats() {
    const struct {
        rhi::VertexFormat rhi_format;
        wg::WgpuVertexFormat wgpu_format;
        std::uint32_t size;
    } cases[] = {
        {rhi::VertexFormat::Float, wg::WgpuVertexFormat::Float32, 4},
        {rhi::VertexFormat::Float2, wg::WgpuVertexFormat::Float32x2, 8},
        {rhi::VertexFormat::Float3, wg::WgpuVertexFormat::Float32x3, 12},
        {rhi::VertexFormat::Float4, wg::WgpuVertexFormat::Float32x4, 16},
        {rhi::VertexFormat::UByte4Norm, wg::WgpuVertexFormat::Unorm8x4, 4},
    };
    for (const auto& c : cases) {
        expect(wg::vertex_format(c.rhi_format) == c.wgpu_format, "vertex format mapping");
        expect(wg::vertex_format_size(c.rhi_format) == c.size, "vertex format size");
    }
}

void test_blend() {
    const wg::WgpuBlendState none = wg::blend_state(rhi::BlendMode::None);
    expect(!none.enabled, "no blend is disabled");
    const wg::WgpuBlendState alpha = wg::blend_state(rhi::BlendMode::Alpha);
    expect(alpha.enabled, "alpha blend is enabled");
    expect(alpha.color.src == wg::WgpuBlendFactor::SrcAlpha &&
               alpha.color.dst == wg::WgpuBlendFactor::OneMinusSrcAlpha &&
               alpha.color.op == wg::WgpuBlendOp::Add,
           "alpha color factors match the Vulkan backend");
    expect(alpha.alpha.src == wg::WgpuBlendFactor::One &&
               alpha.alpha.dst == wg::WgpuBlendFactor::OneMinusSrcAlpha &&
               alpha.alpha.op == wg::WgpuBlendOp::Add,
           "alpha alpha factors match the Vulkan backend");
}

void test_sizes_and_alignment() {
    expect(wg::is_supported_sample_count(1) && wg::is_supported_sample_count(4) &&
               !wg::is_supported_sample_count(2) && !wg::is_supported_sample_count(8),
           "sample counts are 1 and 4 only");
    expect(wg::align_up(0, 256) == 0 && wg::align_up(1, 256) == 256 &&
               wg::align_up(256, 256) == 256 && wg::align_up(257, 256) == 512,
           "align_up");
    expect(wg::padded_bytes_per_row(1, rhi::TextureFormat::R8Unorm) == 256, "1px r8 row");
    expect(wg::padded_bytes_per_row(64, rhi::TextureFormat::R8G8B8A8Unorm) == 256, "64px rgba row");
    expect(wg::padded_bytes_per_row(65, rhi::TextureFormat::R8G8B8A8Unorm) == 512, "65px rgba row");
    expect(wg::readback_buffer_size({100, 10}, rhi::TextureFormat::R8G8B8A8Unorm) == 512u * 10,
           "readback buffer size");
    expect(wg::push_constant_uniform_size(0) == 0, "no push constants");
    expect(wg::push_constant_uniform_size(112) == 112, "112-byte block is already 16-aligned");
    expect(wg::push_constant_uniform_size(100) == 112, "uniform size rounds up to 16");
    expect(wg::uniform_slice_stride(112) == 256 && wg::uniform_slice_stride(257) == 512,
           "uniform slice stride");

    const rhi::Scissor inside = wg::clamp_scissor({10, 20, 30, 40}, {100, 100});
    expect(inside.x == 10 && inside.y == 20 && inside.width == 30 && inside.height == 40,
           "scissor inside target is unchanged");
    const rhi::Scissor overhang = wg::clamp_scissor({-5, -5, 20, 20}, {10, 10});
    expect(overhang.x == 0 && overhang.y == 0 && overhang.width == 10 && overhang.height == 10,
           "scissor is clamped to the target");
    const rhi::Scissor outside = wg::clamp_scissor({200, 200, 5, 5}, {100, 100});
    expect(outside.width == 0 && outside.height == 0, "scissor outside the target is empty");
}

void test_vertex_layout_validation() {
    using Error = wg::VertexLayoutError;
    // The real batched 2D layout must pass.
    rhi::VertexBufferLayout batched;
    batched.stride = tessera::rendering::gpu::kVertex2DStride;
    batched.attributes = tessera::rendering::gpu::kVertex2DAttributes;
    batched.attribute_count = tessera::rendering::gpu::kVertex2DAttributeCount;
    expect(wg::validate_vertex_layout(batched) == Error::None, "batched2d layout is valid");

    const rhi::VertexAttribute ok[] = {{0, 0, rhi::VertexFormat::Float2}};
    rhi::VertexBufferLayout layout{8, ok, 1};
    expect(wg::validate_vertex_layout(layout) == Error::None, "simple layout is valid");

    expect(wg::validate_vertex_layout({8, nullptr, 0}) == Error::MissingAttributes, "no attrs");
    expect(wg::validate_vertex_layout({6, ok, 1}) == Error::StrideUnaligned, "stride % 4");
    expect(wg::validate_vertex_layout({4096, ok, 1}) == Error::StrideTooLarge, "stride limit");
    expect(wg::validate_vertex_layout({4, ok, 1}) == Error::AttributeOutOfStride,
           "attribute past stride");

    const rhi::VertexAttribute bad_loc[] = {{16, 0, rhi::VertexFormat::Float}};
    expect(wg::validate_vertex_layout({8, bad_loc, 1}) == Error::LocationOutOfRange, "location");
    const rhi::VertexAttribute dup[] = {{1, 0, rhi::VertexFormat::Float},
                                        {1, 4, rhi::VertexFormat::Float}};
    expect(wg::validate_vertex_layout({8, dup, 2}) == Error::DuplicateLocation, "duplicate loc");
    const rhi::VertexAttribute unaligned[] = {{0, 2, rhi::VertexFormat::Float}};
    expect(wg::validate_vertex_layout({8, unaligned, 1}) == Error::OffsetUnaligned, "offset");
    rhi::VertexBufferLayout many{128, ok, 17};
    expect(wg::validate_vertex_layout(many) == Error::TooManyAttributes, "too many attributes");
}

void test_shader_registry() {
    wg::ShaderRegistry registry;
    expect(registry.find("batched2d") == nullptr && registry.find("") == nullptr, "empty registry");
    expect(!registry.add("", wg::WgslProgram{"ignored"}) && registry.size() == 0,
           "empty id means SPIR-V words and is rejected");

    expect(registry.add("custom", wg::WgslProgram{"first"}), "non-empty id registers");
    expect(registry.size() == 1 && registry.find("custom") != nullptr, "registered id resolves");
    expect(registry.find("custom")->source == "first" &&
               registry.find("custom")->vertex_entry == "vs_main",
           "program contents and default entry points");
    expect(registry.find("Custom") == nullptr && registry.find("custom2") == nullptr,
           "ids compare exactly");

    // The id is copied: a caller's temporary buffer may go away after registration.
    {
        std::string temporary = "transient";
        registry.add(temporary, wg::WgslProgram{"kept"});
    }
    expect(registry.find("transient") != nullptr && registry.find("transient")->source == "kept",
           "registry owns its ids");

    registry.add("custom", wg::WgslProgram{"second"});
    expect(registry.size() == 2 && registry.find("custom")->source == "second",
           "re-registering replaces");
}

void test_builtin_registry() {
    const wg::ShaderRegistry registry = wg::builtin_shader_registry();
    const wg::WgslProgram* program = registry.find(tessera::rendering::gpu::kBatched2dShaderId);
    expect(program != nullptr, "built-in registry resolves batched2d");
    expect(program != nullptr && program->source == wg::batched2d_wgsl_program().source,
           "built-in batched2d is the embedded WGSL");
    expect(registry.find("") == nullptr, "SPIR-V-only descriptors never resolve to WGSL");
}
void test_builtin_wgsl() {
    const wg::WgslProgram program = wg::batched2d_wgsl_program();
    const std::string_view source = program.source;
    expect(!source.empty(), "batched2d WGSL is embedded");
    expect(source.find("fn vs_main") != std::string_view::npos &&
               source.find("fn fs_main") != std::string_view::npos,
           "entry points exist");
    expect(source.find("@group(0) @binding(0) var<uniform>") != std::string_view::npos &&
               source.find("@group(0) @binding(1)") != std::string_view::npos &&
               source.find("@group(0) @binding(2)") != std::string_view::npos,
           "binding layout matches kBinding* constants");
    expect(wg::kBindingPushConstants == 0 && wg::kBindingTexture == 1 && wg::kBindingSampler == 2,
           "binding constants");
    // One @location per Vertex2D attribute.
    for (std::uint32_t i = 0; i < tessera::rendering::gpu::kVertex2DAttributeCount; ++i) {
        const std::string needle = "@location(" + std::to_string(i) + ") ";
        expect(source.find(needle) != std::string_view::npos, "vertex attribute location exists");
    }
}

} // namespace

int main() {
    test_texture_formats();
    test_usage_flags();
    test_sampler_and_ops();
    test_vertex_formats();
    test_blend();
    test_sizes_and_alignment();
    test_vertex_layout_validation();
    test_shader_registry();
    test_builtin_registry();
    test_builtin_wgsl();
    if (g_failures != 0) {
        std::cerr << "webgpu_mapping: " << g_failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
