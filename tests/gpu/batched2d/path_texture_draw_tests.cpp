// SPDX-License-Identifier: MIT
//
// Path raster upload -> batched2d textured draw. The null-device section always runs and proves the
// cache texture reaches the batched2d textured-quad path (bind + one 6-vertex draw) without a GPU.
// With Vulkan available it renders a cached path texture into offscreen targets and checks the
// readback: UNORM opaque bytes are exact, straight-alpha blending matches the GPU blend equation,
// and an sRGB texture into an sRGB target round-trips (D2), with two runs byte-identical.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/null/null_device.hpp"
#include "tessera/rendering/path/path_rasterizer.hpp"
#include "tessera/rendering/path/path_texture_cache.hpp"
#include "tessera/rendering/path/path_texture_key.hpp"
#include "tessera/rendering/path/svg_loader.hpp"

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
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace path = tessera::rendering::path;
namespace rhi = tessera::rendering::rhi;

constexpr std::uint32_t kBox = 64;

[[nodiscard]] bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "path_texture_draw: " << message << "\n";
    }
    return condition;
}

// Fills every pixel with one straight-alpha color: enough to verify upload bytes, draw plumbing and
// the blend equation without depending on ThorVG (optional) or on a specific rasterizer.
class SolidRasterizer final : public path::PathRasterizer {
public:
    explicit SolidRasterizer(path::Rgba8 color)
        : color_(color) {}

    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return "solid";
    }

    [[nodiscard]] std::expected<path::RasterStats, path::RasterError>
    rasterize(std::span<const path::PathShape> shapes, const path::RasterTarget& target) override {
        ++calls;
        if (!target.valid()) {
            return std::unexpected(path::RasterError::invalid_target);
        }
        for (std::uint32_t y = 0; y < target.height; ++y) {
            std::uint8_t* row =
                target.pixels.data() + static_cast<std::size_t>(y) * target.stride_bytes;
            for (std::uint32_t x = 0; x < target.width; ++x) {
                row[x * 4U + 0U] = color_.r;
                row[x * 4U + 1U] = color_.g;
                row[x * 4U + 2U] = color_.b;
                row[x * 4U + 3U] = color_.a;
            }
        }
        return path::RasterStats{static_cast<std::uint32_t>(shapes.size())};
    }

    std::uint32_t calls = 0;

private:
    path::Rgba8 color_;
};

[[nodiscard]] path::PathShape box_shape() {
    path::Path path;
    path.segments.push_back({path::SegmentKind::move_to, {0.0f, 0.0f}, {}, {}});
    path.segments.push_back({path::SegmentKind::line_to, {static_cast<float>(kBox), 0.0f}, {}, {}});
    path.segments.push_back(
        {path::SegmentKind::line_to, {static_cast<float>(kBox), static_cast<float>(kBox)}, {}, {}});
    path.segments.push_back({path::SegmentKind::line_to, {0.0f, static_cast<float>(kBox)}, {}, {}});
    path.segments.push_back({path::SegmentKind::close, {}, {}, {}});
    path::PaintStyle paint;
    paint.fill =
        path::Rgba8{0, 0, 0, 255}; // the rasterizer overrides pixels; paint is the key input
    return path::PathShape{std::move(path), paint};
}

[[nodiscard]] path::PathTextureKey box_key(std::uint64_t id, std::uint32_t scale_milli,
                                           const path::PathShape& shape) {
    return path::make_path_texture_key(id, 1, kBox, kBox, scale_milli, shape);
}

[[nodiscard]] std::vector<std::uint8_t> read_svg_fixture() {
    // The fixture directory is derived from this source file (CMake compiles with an absolute
    // path), so the test needs no extra compile definition and works from any build directory and
    // from a clean-checkout worktree.
    const std::filesystem::path source(__FILE__);
    const std::filesystem::path path =
        source.parent_path().parent_path().parent_path() / "fixtures" / "svg" / "stroke_path.svg";
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "path_texture_draw: cannot open " << path.string() << "\n";
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>());
}

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

// Manual measurement mode (`--measure`), not part of the CTest run: upload cost for a 512x512 path
// texture and steady-state cache-hit cost on a real device. The rasterizer is a solid fill, so the
// miss number isolates rasterize + atlas-free upload into the RHI.
void measure_upload_and_hits(rhi::Device& device) {
    constexpr std::uint32_t kSize = 512;
    path::Path path;
    path.segments.push_back({path::SegmentKind::move_to, {0.0f, 0.0f}, {}, {}});
    path.segments.push_back(
        {path::SegmentKind::line_to, {static_cast<float>(kSize), 0.0f}, {}, {}});
    path.segments.push_back({path::SegmentKind::line_to,
                             {static_cast<float>(kSize), static_cast<float>(kSize)},
                             {},
                             {}});
    path.segments.push_back(
        {path::SegmentKind::line_to, {0.0f, static_cast<float>(kSize)}, {}, {}});
    path.segments.push_back({path::SegmentKind::close, {}, {}, {}});
    path::PaintStyle paint;
    paint.fill = path::Rgba8{10, 200, 60, 255};
    const path::PathShape shape{std::move(path), paint};

    SolidRasterizer rasterizer{path::Rgba8{10, 200, 60, 255}};
    path::PathTextureCacheConfig config;
    config.max_entries = 8;
    config.max_bytes = 64ULL * 1024ULL * 1024ULL;
    path::PathTextureCache cache(device, rasterizer, config);

    const auto key = path::make_path_texture_key(9, 1, kSize, kSize, 1000, shape);
    const auto start = std::chrono::steady_clock::now();
    const auto first = cache.acquire(key, shape);
    const auto first_done = std::chrono::steady_clock::now();
    if (!first.has_value()) {
        std::cerr << "path_texture_draw: measurement upload failed\n";
        return;
    }
    const double first_ms = std::chrono::duration<double, std::milli>(first_done - start).count();

    constexpr int kHitIterations = 10000;
    double hit_total_ms = 0.0;
    for (int i = 0; i < kHitIterations; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        const auto hit = cache.acquire(key, shape);
        const auto end = std::chrono::steady_clock::now();
        hit_total_ms += std::chrono::duration<double, std::milli>(end - begin).count();
        if (!hit.has_value()) {
            std::cerr << "path_texture_draw: measurement hit failed\n";
            return;
        }
    }

    constexpr int kUploadIterations = 10;
    double upload_total_ms = 0.0;
    for (int i = 0; i < kUploadIterations; ++i) {
        cache.invalidate_all();
        const auto begin = std::chrono::steady_clock::now();
        const auto uploaded = cache.acquire(key, shape);
        const auto end = std::chrono::steady_clock::now();
        upload_total_ms += std::chrono::duration<double, std::milli>(end - begin).count();
        if (!uploaded.has_value()) {
            std::cerr << "path_texture_draw: measurement re-upload failed\n";
            return;
        }
    }

    std::cout << std::fixed << std::setprecision(4)
              << "path_texture_draw: measure 512x512 first_acquire_ms=" << first_ms
              << " upload_again_ms_avg=" << (upload_total_ms / kUploadIterations)
              << " hit_ms_avg=" << (hit_total_ms / kHitIterations)
              << " texture_bytes=" << (512ULL * 512ULL * 4ULL) << "\n";
}

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

// End-to-end SVG asset path: decode -> rasterize -> cache -> textured quad. The null-device
// assertions run in every configuration; the readback pixels run when Vulkan is part of the build.
bool svg_end_to_end() {
    const auto bytes = read_svg_fixture();
    if (!expect(!bytes.empty(), "SVG fixture is readable")) {
        return false;
    }
    const auto document = path::load_svg(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
    if (!path::svg_backend_available()) {
        return expect(!document.has_value() &&
                          document.error() == path::SvgError::backend_unavailable,
                      "ThorVG OFF returns backend_unavailable from the SVG API (no link error)");
    }
    if (!expect(document.has_value(), "SVG fixture decodes")) {
        return false;
    }
    if (!expect(document->width == kBox && document->height == kBox && document->shapes.size() == 1,
                "SVG fixture decodes one 64x64 shape")) {
        return false;
    }

    auto rasterizer = path::make_path_rasterizer(path::PathBackend::thorvg);
    if (!expect(rasterizer != nullptr, "ThorVG rasterizer is available for the SVG path")) {
        return false;
    }
    const auto& shape = document->shapes[0];
    const auto key =
        path::make_path_texture_key(7, 1, document->width, document->height, 1000, shape);

    auto null_device = tessera::rendering::null::create_null_device(rhi::DeviceDesc{});
    path::PathTextureCache null_cache(*null_device, *rasterizer, {});
    const auto cached = null_cache.acquire(key, shape);
    bool ok = expect(cached.has_value() && cached->texture != nullptr,
                     "SVG path uploads through the texture cache");
    if (ok) {
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
                        "SVG texture draws as one 6-vertex textured quad") &&
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
    const auto uploaded = cache.acquire(key, shape);
    if (!expect(uploaded.has_value(), "Vulkan: SVG path texture uploads")) {
        return false;
    }
    const RenderSpec spec;
    const auto first = render_and_readback(*device, *uploaded->texture, spec);
    const auto second = render_and_readback(*device, *uploaded->texture, spec);
    // stroke_path.svg: #2e8b57 triangle at fill-opacity 0.75 over an opaque black clear.
    const std::array<int, 4> inside = {static_cast<int>(std::lround(46.0 * 191.0 / 255.0)),
                                       static_cast<int>(std::lround(139.0 * 191.0 / 255.0)),
                                       static_cast<int>(std::lround(87.0 * 191.0 / 255.0)), 255};
    ok = expect(vk_pixel_near(first, 40, 20, inside, 2),
                "Vulkan: SVG fill pixel matches the decoded fill/alpha") &&
         expect(vk_pixel_near(first, 4, 50, {0, 0, 0, 255}, 0),
                "Vulkan: SVG pixel outside the triangle is the clear color") &&
         expect(first == second, "Vulkan: two SVG renders are byte-identical") && ok;
#endif
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    const bool measure = argc > 1 && std::string_view(argv[1]) == "--measure";
    bool ok = true;
    ok = null_device_draw() && ok;
    ok = svg_end_to_end() && ok;
#ifdef TESSERA_TEST_HAS_VULKAN
    if (measure) {
        rhi::DeviceDesc device_desc;
        device_desc.backend = rhi::Backend::Vulkan;
        try {
            const auto device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
            measure_upload_and_hits(*device);
        } catch (const std::exception& error) {
            std::cerr << "path_texture_draw: measure skipped (" << error.what() << ")\n";
        }
        return ok ? 0 : 1;
    }
    ok = vulkan_draw() && ok;
#else
    static_cast<void>(measure);
    std::cout << "path_texture_draw: Vulkan draw cases not built (Vulkan backend OFF)\n";
#endif
    if (!ok) {
        std::cerr << "path_texture_draw: FAILED\n";
        return 1;
    }
    std::cout << "path_texture_draw: ok\n";
    return 0;
}
