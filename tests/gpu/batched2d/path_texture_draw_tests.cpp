// SPDX-License-Identifier: MIT
//
// Path raster upload -> batched2d textured draw. The null-device section always runs and proves the
// cache texture reaches the batched2d textured-quad path (bind + one 6-vertex draw) without a GPU.
// With Vulkan available it renders a cached path texture into offscreen targets and checks the
// readback: UNORM opaque bytes are exact, straight-alpha blending matches the GPU blend equation,
// and an sRGB texture into an sRGB target round-trips (D2), with two runs byte-identical.

#include "path_texture_draw_support.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/null/null_device.hpp"
#include "tessera/rendering/path/path_rasterizer.hpp"
#include "tessera/rendering/path/path_texture_cache.hpp"
#include "tessera/rendering/path/path_texture_key.hpp"
#include "tessera/rendering/path/svg_document_cache.hpp"
#include "tessera/rendering/path/svg_loader.hpp"
#include "tessera/rendering/path/svg_texture.hpp"

#ifdef TESSERA_TEST_HAS_VULKAN
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#endif

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using tessera::test::path_draw::box_key;
using tessera::test::path_draw::box_shape;
using tessera::test::path_draw::expect;
using tessera::test::path_draw::kBox;
using tessera::test::path_draw::read_svg_fixture;
using tessera::test::path_draw::SolidRasterizer;
namespace path = tessera::rendering::path;
namespace rhi = tessera::rendering::rhi;

// Null-device cover: the cached texture reaches batched2d's textured-quad path exactly once.
bool null_device_draw() {
    auto device = tessera::rendering::null::create_null_device(rhi::DeviceDesc{});
    SolidRasterizer rasterizer{path::Rgba8{40, 80, 120, 255}};
    path::PathTextureCache cache(*device, rasterizer, {});
    const auto shape = box_shape();
    const auto cached = cache.acquire(box_key(1, 1000, shape), shape);
    if (!expect(cached.has_value() && cached->texture != nullptr, "cache uploads a path texture")) {
        return false;
    }
    const auto hit = cache.acquire(box_key(1, 1000, shape), shape);
    bool ok =
        expect(hit.has_value() && hit->texture == cached->texture, "second acquire is a hit") &&
        expect(cache.stats().hits == 1 && cache.stats().uploads == 1,
               "cache reports one hit and one upload");

    auto target = device->create_render_target(rhi::RenderTargetDesc{{kBox, kBox}});
    auto white = device->create_texture(rhi::TextureDesc{{1, 1}});
    auto pipeline = device->create_pipeline(rhi::PipelineDesc{});
    auto commands = device->create_command_list();
    if (!expect(target && white && pipeline && commands, "null device creates draw resources")) {
        return false;
    }

    tessera::rendering::gpu::Batched2DRenderer renderer(*device, *pipeline, *white);
    const rhi::RenderTargetDesc desc{{kBox, kBox}};
    renderer.begin(*commands, *target, desc);
    renderer.push_texture({0.0f, 0.0f, static_cast<float>(kBox), static_cast<float>(kBox)},
                          *cached->texture, {0.0f, 0.0f, 1.0f, 1.0f}, rhi::rgba8(255, 255, 255));
    renderer.end();
    commands->submit();

    auto* null_commands = dynamic_cast<tessera::rendering::null::NullCommandList*>(commands.get());
    if (!expect(null_commands != nullptr, "null command list type")) {
        return false;
    }
    const auto& stats = null_commands->stats();
    ok = expect(stats.draws == 1, "one batched2d draw for the path texture") && ok;
    ok = expect(stats.vertices == 6, "a path texture is one quad (6 vertices)") && ok;
    ok = expect(stats.texture_binds >= 1, "the path texture is bound") && ok;
    ok = expect(null_commands->submitted(), "the draw was submitted") && ok;
    return ok;
}

#ifdef TESSERA_TEST_HAS_VULKAN

struct RenderSpec {
    rhi::TextureFormat target_format = rhi::TextureFormat::R8G8B8A8Unorm;
    rhi::Color clear{0.0f, 0.0f, 0.0f, 1.0f};
    rhi::Extent2D extent{kBox, kBox};
};

// Draws one texture over the whole target and returns the 8-bit readback.
std::vector<std::uint8_t> render_and_readback(rhi::Device& device, rhi::Texture& texture,
                                              const RenderSpec& spec) {
    rhi::RenderTargetDesc target_desc;
    target_desc.extent = spec.extent;
    target_desc.format = spec.target_format;
    target_desc.clear_color = spec.clear;
    auto target = device.create_render_target(target_desc);

    auto pipeline_desc = tessera::rendering::vulkan::batched2d_pipeline_desc();
    pipeline_desc.color_format = spec.target_format;
    auto pipeline = device.create_pipeline(pipeline_desc);

    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device.create_texture(white_desc);

    tessera::rendering::gpu::Batched2DRenderer renderer(device, *pipeline, *white);
    auto commands = device.create_command_list();
    renderer.begin(*commands, *target, target_desc);
    renderer.push_texture(
        {0.0f, 0.0f, static_cast<float>(spec.extent.width), static_cast<float>(spec.extent.height)},
        texture, {0.0f, 0.0f, 1.0f, 1.0f}, rhi::rgba8(255, 255, 255));
    renderer.end();
    commands->submit();
    device.wait_idle();

    const std::size_t bytes = static_cast<std::size_t>(spec.extent.width) * spec.extent.height * 4;
    auto readback = device.create_buffer({bytes, rhi::BufferUsage::Readback});
    auto copy_commands = device.create_command_list();
    copy_commands->copy_target_to_buffer(*target, *readback);
    copy_commands->submit();
    device.wait_idle();

    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

// The batched2d fragment shader multiplies the vertex color by the sampled texel; Vulkan blends
// with SRC_ALPHA / ONE_MINUS_SRC_ALPHA, so a straight-alpha texel over an opaque clear gives `src *
// a + dst * (1 - a)`.
[[nodiscard]] int blended(int src, int dst, int alpha) {
    const double a = static_cast<double>(alpha) / 255.0;
    return static_cast<int>(
        std::lround(static_cast<double>(src) * a + static_cast<double>(dst) * (1.0 - a)));
}

bool vk_pixels_near(const std::vector<std::uint8_t>& pixels, const std::array<int, 4>& expected,
                    int tolerance) {
    for (std::size_t offset = 0; offset + 3 < pixels.size(); offset += 4) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            if (std::abs(static_cast<int>(pixels[offset + channel]) - expected[channel]) >
                tolerance) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool vk_pixel_near(const std::vector<std::uint8_t>& pixels, std::uint32_t x,
                                 std::uint32_t y, const std::array<int, 4>& expected,
                                 int tolerance) {
    const std::size_t offset = (static_cast<std::size_t>(y) * kBox + x) * 4U;
    if (offset + 3U >= pixels.size()) {
        return false;
    }
    for (std::size_t channel = 0; channel < 4; ++channel) {
        if (std::abs(static_cast<int>(pixels[offset + channel]) - expected[channel]) > tolerance) {
            return false;
        }
    }
    return true;
}

bool vulkan_draw() {
    rhi::DeviceDesc device_desc;
    device_desc.backend = rhi::Backend::Vulkan;
    std::unique_ptr<rhi::Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "path_texture_draw: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return true;
    }

    // 1) UNORM opaque: the texture bytes survive the upload and the draw exactly.
    SolidRasterizer opaque{path::Rgba8{10, 200, 60, 255}};
    path::PathTextureCache opaque_cache(*device, opaque, {});
    const auto shape = box_shape();
    const auto opaque_key = box_key(2, 1000, shape);
    const auto opaque_texture = opaque_cache.acquire(opaque_key, shape);
    if (!expect(opaque_texture.has_value(), "Vulkan: opaque path texture uploads")) {
        return false;
    }
    const RenderSpec unorm_spec;
    const auto first = render_and_readback(*device, *opaque_texture->texture, unorm_spec);
    const auto second = render_and_readback(*device, *opaque_texture->texture, unorm_spec);
    bool ok = expect(vk_pixels_near(first, {10, 200, 60, 255}, 0),
                     "Vulkan: UNORM textured quad reproduces the cached bytes") &&
              expect(first == second, "Vulkan: two renders are byte-identical") &&
              expect(opaque_cache.acquire(opaque_key, shape).has_value() &&
                         opaque_cache.stats().hits == 1,
                     "Vulkan: second cache acquire is a hit");

    // 2) Straight-alpha blend into an opaque clear.
    SolidRasterizer translucent{path::Rgba8{200, 40, 40, 128}};
    path::PathTextureCache blend_cache(*device, translucent, {});
    const auto blend_texture = blend_cache.acquire(box_key(3, 1000, shape), shape);
    if (!expect(blend_texture.has_value(), "Vulkan: translucent path texture uploads")) {
        return false;
    }
    RenderSpec blend_spec;
    blend_spec.clear = rhi::rgba8(0, 0, 255);
    const auto blended_pixels = render_and_readback(*device, *blend_texture->texture, blend_spec);
    const std::array<int, 4> expected = {blended(200, 0, 128), blended(40, 0, 128),
                                         blended(40, 255, 128), blended(255, 255, 128)};
    ok = expect(vk_pixels_near(blended_pixels, expected, 2),
                "Vulkan: straight-alpha path texture blends with the GPU blend equation") &&
         ok;

    // 3) D2 sRGB round-trip: an sRGB texture sampled into an sRGB target re-encodes to the stored
    //    encoded bytes instead of encoding them twice.
    try {
        SolidRasterizer srgb_rasterizer{path::Rgba8{32, 96, 200, 255}};
        path::PathTextureCacheConfig srgb_config;
        srgb_config.format = rhi::TextureFormat::R8G8B8A8Srgb;
        path::PathTextureCache srgb_cache(*device, srgb_rasterizer, srgb_config);
        const auto srgb_texture = srgb_cache.acquire(box_key(4, 1000, shape), shape);
        if (!expect(srgb_texture.has_value(), "Vulkan: sRGB path texture uploads")) {
            return false;
        }
        RenderSpec srgb_spec;
        srgb_spec.target_format = rhi::TextureFormat::R8G8B8A8Srgb;
        const auto srgb_pixels = render_and_readback(*device, *srgb_texture->texture, srgb_spec);
        ok = expect(vk_pixels_near(srgb_pixels, {32, 96, 200, 255}, 2),
                    "Vulkan: sRGB texture into an sRGB target round-trips the encoded bytes") &&
             ok;
    } catch (const std::exception& error) {
        std::cout << "path_texture_draw: sRGB case skipped (" << error.what() << ")\n";
    }

    // 4) 2x raster scale: the cache rasterizes a 128x128 texture for a 64x64 box; the draw
    //    downsamples deterministically and the two runs match byte-for-byte.
    SolidRasterizer scaled_rasterizer{path::Rgba8{70, 140, 210, 255}};
    path::PathTextureCache scaled_cache(*device, scaled_rasterizer, {});
    const auto scaled_texture = scaled_cache.acquire(box_key(5, 2000, shape), shape);
    if (!expect(scaled_texture.has_value() && scaled_texture->extent.width == kBox * 2 &&
                    scaled_texture->extent.height == kBox * 2,
                "Vulkan: 2x scale rasterizes at 2x extent")) {
        return false;
    }
    const auto scaled_first = render_and_readback(*device, *scaled_texture->texture, unorm_spec);
    const auto scaled_second = render_and_readback(*device, *scaled_texture->texture, unorm_spec);
    ok = expect(scaled_first == scaled_second, "Vulkan: 2x downscale is deterministic") && ok;
    ok = expect(vk_pixels_near(scaled_first, {70, 140, 210, 255}, 1),
                "Vulkan: uniform 2x downscale preserves the color") &&
         ok;

    if (!ok) {
        std::cerr << "path_texture_draw: Vulkan cases failed\n";
    }
    return ok;
}

#endif // TESSERA_TEST_HAS_VULKAN

// End-to-end SVG asset path: decode once into a document cache, rasterize the whole document into
// one texture, then draw it as a textured quad. The null-device assertions run in every
// configuration; the readback pixels run when Vulkan is part of the build.
bool svg_end_to_end() {
    const auto bytes = read_svg_fixture("icon_shapes.svg");
    if (!expect(!bytes.empty(), "SVG fixture is readable")) {
        return false;
    }
    const bool available = path::svg_backend_available();
    path::SvgDocumentCache documents;
    const path::SvgDocumentKey document_key =
        path::make_svg_document_key(std::span<const std::uint8_t>(bytes));
    const auto document = documents.acquire(document_key, std::span<const std::uint8_t>(bytes));
    if (!available) {
        return expect(!document.has_value() &&
                          document.error() == path::SvgError::backend_unavailable,
                      "ThorVG OFF returns backend_unavailable from the document cache (no link "
                      "error)");
    }
    if (!expect(document.has_value(), "SVG fixture decodes")) {
        return false;
    }
    if (!expect(document.value()->width == kBox && document.value()->height == kBox &&
                    document.value()->shapes.size() == 2,
                "icon_shapes decodes two 64x64 shapes")) {
        return false;
    }
    const auto again = documents.acquire(document_key, std::span<const std::uint8_t>());
    bool ok = expect(again.has_value() && again.value() == document.value() &&
                         documents.stats().decodes == 1,
                     "the document is decoded once for every rasterization");

    auto rasterizer = path::make_path_rasterizer(path::PathBackend::thorvg);
    if (!expect(rasterizer != nullptr, "ThorVG rasterizer is available for the SVG path")) {
        return false;
    }
    const path::SvgTextureKey key = path::make_svg_texture_key(7, 1, *document.value());

    auto null_device = tessera::rendering::null::create_null_device(rhi::DeviceDesc{});
    path::PathTextureCache null_cache(*null_device, *rasterizer, {});
    const auto cached = path::acquire_svg_texture(null_cache, key, *document.value());
    ok = expect(cached.has_value() && cached->texture != nullptr,
                "the SVG document uploads as one texture") &&
         ok;
    if (ok) {
        const auto cached_again = path::acquire_svg_texture(null_cache, key, *document.value());
        ok = expect(cached_again.has_value() && cached_again->texture == cached->texture &&
                        null_cache.stats().uploads == 1,
                    "the second document acquire is a texture-cache hit") &&
             ok;

        auto target = null_device->create_render_target(rhi::RenderTargetDesc{{kBox, kBox}});
        auto white = null_device->create_texture(rhi::TextureDesc{{1, 1}});
        auto pipeline = null_device->create_pipeline(rhi::PipelineDesc{});
        auto commands = null_device->create_command_list();
        ok = expect(target && white && pipeline && commands, "null device draw resources") && ok;
        if (ok) {
            tessera::rendering::gpu::Batched2DRenderer renderer(*null_device, *pipeline, *white);
            const rhi::RenderTargetDesc desc{{kBox, kBox}};
            renderer.begin(*commands, *target, desc);
            renderer.push_texture({0.0f, 0.0f, static_cast<float>(kBox), static_cast<float>(kBox)},
                                  *cached->texture, {0.0f, 0.0f, 1.0f, 1.0f},
                                  rhi::rgba8(255, 255, 255));
            renderer.end();
            commands->submit();
            auto* null_commands =
                dynamic_cast<tessera::rendering::null::NullCommandList*>(commands.get());
            ok = expect(null_commands != nullptr && null_commands->stats().draws == 1 &&
                            null_commands->stats().vertices == 6 &&
                            null_commands->stats().texture_binds >= 1,
                        "the document texture draws as one 6-vertex quad") &&
                 ok;
        }
    }

#ifdef TESSERA_TEST_HAS_VULKAN
    rhi::DeviceDesc device_desc;
    device_desc.backend = rhi::Backend::Vulkan;
    std::unique_ptr<rhi::Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "path_texture_draw: SVG Vulkan readback skipped (" << error.what() << ")\n";
        return ok;
    }
    path::PathTextureCache cache(*device, *rasterizer, {});
    const auto uploaded = path::acquire_svg_texture(cache, key, *document.value());
    if (!expect(uploaded.has_value(), "Vulkan: SVG document texture uploads")) {
        return false;
    }
    const RenderSpec spec;
    const auto first = render_and_readback(*device, *uploaded->texture, spec);
    const auto second = render_and_readback(*device, *uploaded->texture, spec);
    // icon_shapes.svg: an opaque #3366cc rect and a #cc6633 circle at ~50% fill-opacity.
    const path::PathShape& circle = document.value()->shapes[1];
    const int alpha = circle.paint.fill.has_value() ? circle.paint.fill->a : 128;
    const std::array<int, 4> circle_pixel = {static_cast<int>(std::lround(204.0 * alpha / 255.0)),
                                             static_cast<int>(std::lround(102.0 * alpha / 255.0)),
                                             static_cast<int>(std::lround(51.0 * alpha / 255.0)),
                                             255};
    ok = expect(vk_pixel_near(first, 16, 16, {51, 102, 204, 255}, 0),
                "Vulkan: opaque rect pixel matches the SVG fill") &&
         expect(vk_pixel_near(first, 4, 4, {0, 0, 0, 255}, 0),
                "Vulkan: pixel outside the shapes is the clear color") &&
         expect(vk_pixel_near(first, 40, 40, circle_pixel, 2),
                "Vulkan: translucent circle pixel matches fill-opacity") &&
         expect(first == second, "Vulkan: two document renders are byte-identical") && ok;
#endif
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = null_device_draw() && ok;
    ok = svg_end_to_end() && ok;
#ifdef TESSERA_TEST_HAS_VULKAN
    ok = vulkan_draw() && ok;
#else
    std::cout << "path_texture_draw: Vulkan draw cases not built (Vulkan backend OFF)\n";
#endif
    if (!ok) {
        std::cerr << "path_texture_draw: FAILED\n";
        return 1;
    }
    std::cout << "path_texture_draw: ok\n";
    return 0;
}
