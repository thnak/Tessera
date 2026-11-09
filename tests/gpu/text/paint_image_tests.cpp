// SPDX-License-Identifier: MIT
//
// Real-Vulkan raster-image paint readback: an embedded PNG is set as PaintData on scene nodes,
// painted by paint_scene -> Batched2DSink (ImageDecodeCache + ImageTextureCache) ->
// Batched2DRenderer, and read back. Asserts image pixels inside the content box, background
// outside, one decode and one upload per (content, extent), tint/opacity, missing-asset no-op,
// byte-identical repeat, the sRGB target path with validation enabled, and prints 1/10/100-node
// measurements.

#include "paint_image_gpu_tests_assets.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/image/image_decode_cache.hpp"
#include "tessera/rendering/image/image_texture_cache.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/ui/assets/asset_bundle.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace image = tessera::rendering::image;
namespace assets = tessera::ui::assets;

using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::rhi::Backend;
using tessera::rendering::rhi::BufferUsage;
using tessera::rendering::rhi::Device;
using tessera::rendering::rhi::DeviceDesc;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::LoadOp;
using tessera::rendering::rhi::RenderTarget;
using tessera::rendering::rhi::RenderTargetDesc;
using tessera::rendering::rhi::StoreOp;
using tessera::rendering::rhi::TextureDesc;
using tessera::rendering::rhi::TextureFormat;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PaintData;
using tessera::ui::PaintKind;
using tessera::ui::paint::Batched2DSink;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::StyleProvider;

constexpr Extent2D kExtent{160, 160};
constexpr std::uint32_t kNodeX = 8;
constexpr std::uint32_t kNodeY = 8;
constexpr std::uint32_t kNodeSize = 64;

[[nodiscard]] bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_image_gpu: " << message << "\n";
    }
    return condition;
}

class StubStyles final : public StyleProvider {
public:
    void set(NodeId id, tessera::ui::style::ComputedStyle style) {
        styles_[id] = std::move(style);
    }
    [[nodiscard]] tessera::ui::style::ComputedStyle compute(const Node& node) override {
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? tessera::ui::style::ComputedStyle{} : found->second;
    }

private:
    std::map<NodeId, tessera::ui::style::ComputedStyle> styles_;
};

[[nodiscard]] tessera::ui::style::ComputedStyle
styled(std::initializer_list<std::pair<const char*, const char*>> values) {
    tessera::ui::style::ComputedStyle style;
    for (const auto& [property, value] : values) {
        style.set(property, value);
    }
    return style;
}

[[nodiscard]] RenderTargetDesc target_desc(TextureFormat format) {
    RenderTargetDesc desc;
    desc.extent = kExtent;
    desc.format = format;
    desc.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    desc.load_op = LoadOp::Clear;
    desc.store_op = StoreOp::Store;
    return desc;
}

[[nodiscard]] std::vector<std::uint8_t> read_target(Device& device, RenderTarget& target) {
    const std::size_t bytes =
        static_cast<std::size_t>(kExtent.width) * kExtent.height * sizeof(std::uint32_t);
    auto readback = device.create_buffer({bytes, BufferUsage::Readback});
    auto commands = device.create_command_list();
    commands->copy_target_to_buffer(target, *readback);
    commands->submit();
    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

[[nodiscard]] std::array<int, 4> pixel_at(const std::vector<std::uint8_t>& pixels, std::uint32_t x,
                                          std::uint32_t y) {
    const std::size_t offset =
        (static_cast<std::size_t>(y) * kExtent.width + x) * sizeof(std::uint32_t);
    return {pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3]};
}

[[nodiscard]] bool pixel_near(const std::vector<std::uint8_t>& pixels, std::uint32_t x,
                              std::uint32_t y, const std::array<int, 4>& expected, int tolerance) {
    const auto actual = pixel_at(pixels, x, y);
    for (std::size_t channel = 0; channel < 4; ++channel) {
        if (std::abs(actual[channel] - expected[channel]) > tolerance) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool region_is_black(const std::vector<std::uint8_t>& pixels, std::uint32_t x0,
                                   std::uint32_t y0, std::uint32_t x1, std::uint32_t y1) {
    for (std::uint32_t y = y0; y < y1; ++y) {
        for (std::uint32_t x = x0; x < x1; ++x) {
            const auto pixel = pixel_at(pixels, x, y);
            if (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] PaintData image_payload(std::string_view asset) {
    PaintData data;
    data.kind = PaintKind::Image;
    data.image.asset = asset;
    return data;
}

struct Scene {
    Node root{1};
    StubStyles styles;

    Scene() {
        root.set_bounds(
            {0.0f, 0.0f, static_cast<float>(kExtent.width), static_cast<float>(kExtent.height)});
        styles.set(1, styled({{"background-color", "black"}}));
    }

    Node& add_image(std::uint32_t id, float x, float y, float size, std::string_view asset) {
        Node& node = root.add_child(std::make_unique<Node>(id));
        node.set_bounds({x, y, size, size});
        node.set_paint_data(image_payload(asset));
        return node;
    }
};

struct RenderHarness {
    assets::AssetBundle bundle{tessera::ui::paint::paint_image_gpu_fixtures::bundle()};
    std::unique_ptr<Device> device;
    image::ImageDecodeCache decoded;
    std::unique_ptr<image::ImageTextureCache> textures;
    std::unique_ptr<tessera::rendering::rhi::Pipeline> pipeline;
    std::unique_ptr<tessera::rendering::rhi::Texture> white;
    std::unique_ptr<Batched2DRenderer> renderer;
    std::unique_ptr<Batched2DSink> sink;

    [[nodiscard]] bool init(TextureFormat format) {
        DeviceDesc desc;
        desc.backend = Backend::Vulkan;
        desc.validation = true;
        try {
            device = tessera::rendering::vulkan::create_vulkan_device(desc);
        } catch (const std::exception& error) {
            std::cout << "paint_image_gpu: skipped (Vulkan device unavailable: " << error.what()
                      << ")\n";
            return false;
        }
        image::ImageTextureCacheConfig cache_config;
        cache_config.format = format;
        textures = std::make_unique<image::ImageTextureCache>(*device, cache_config);
        pipeline =
            device->create_pipeline(tessera::rendering::vulkan::batched2d_pipeline_desc(format, 1));
        const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
        TextureDesc white_desc;
        white_desc.extent = {1, 1};
        white_desc.pixels = white_pixel.data();
        white_desc.row_pitch = 4;
        white = device->create_texture(white_desc);
        renderer = std::make_unique<Batched2DRenderer>(*device, *pipeline, *white,
                                                       Batched2DConfig{4096, false, true});
        tessera::ui::paint::ImagePaintConfig config;
        config.bundle = &bundle;
        config.decoded = &decoded;
        config.textures = textures.get();
        config.device_scale = 1.0f;
        config.texture_format = format;
        sink = std::make_unique<Batched2DSink>(*renderer, nullptr, nullptr,
                                               tessera::ui::paint::VectorPaintConfig{}, config);
        return true;
    }

    [[nodiscard]] std::vector<std::uint8_t> render(Node& root, StyleProvider& styles,
                                                   TextureFormat format, Batched2DSink& sink) {
        const RenderTargetDesc desc = target_desc(format);
        auto target = device->create_render_target(desc);
        auto commands = device->create_command_list();
        renderer->begin(*commands, *target, desc);
        paint_scene(root, styles, sink);
        renderer->end();
        commands->submit();
        device->wait_idle();
        return read_target(*device, *target);
    }
};

bool vulkan_image_readback() {
    RenderHarness harness;
    if (!harness.init(TextureFormat::R8G8B8A8Unorm)) {
        return true; // no device: skip
    }
    Scene scene;
    scene.add_image(2, static_cast<float>(kNodeX), static_cast<float>(kNodeY),
                    static_cast<float>(kNodeSize), "images/checker.png");
    scene.add_image(3, 88.0f, static_cast<float>(kNodeY), static_cast<float>(kNodeSize),
                    "images/checker.png");
    PaintData tinted_data = image_payload("images/checker.png");
    tinted_data.image.tint = tessera::rendering::rhi::rgba8(255, 128, 96, 255);
    tinted_data.image.opacity = 0.5f;
    Node& tinted = scene.add_image(4, 8.0f, 88.0f, 32.0f, "images/checker.png");
    tinted.set_paint_data(std::move(tinted_data));
    scene.add_image(5, 88.0f, 88.0f, 32.0f, "images/missing.png");

    const std::vector<std::uint8_t> first =
        harness.render(scene.root, scene.styles, TextureFormat::R8G8B8A8Unorm, *harness.sink);
    bool ok = expect(pixel_near(first, kNodeX + 16, kNodeY + 16, {255, 0, 0, 255}, 2),
                     "the top-left checker quadrant is red") &&
              expect(pixel_near(first, kNodeX + 48, kNodeY + 16, {0, 255, 0, 255}, 2),
                     "the top-right checker quadrant is green") &&
              expect(pixel_near(first, kNodeX + 16, kNodeY + 48, {0, 0, 255, 255}, 2),
                     "the bottom-left checker quadrant is blue") &&
              expect(pixel_near(first, kNodeX + 48, kNodeY + 48, {128, 128, 128, 255}, 3),
                     "the authored 50%-alpha quadrant blends over the background") &&
              expect(pixel_near(first, 88 + 16, kNodeY + 16, {255, 0, 0, 255}, 2),
                     "a second node sharing the image paints the same pixels") &&
              expect(pixel_near(first, 8 + 8, 88 + 8, {127, 0, 0, 255}, 3),
                     "tint and payload opacity fold into the painted pixels") &&
              expect(region_is_black(first, 88, 88, 120, 120), "a missing asset paints nothing") &&
              expect(pixel_near(first, 150, 150, {0, 0, 0, 255}, 0),
                     "pixels outside every content box stay background");
    const auto& stats = harness.sink->image_stats();
    ok = expect(stats.draws == 3, "three valid nodes emit three image draws") &&
         expect(stats.missing_assets == 1, "the missing asset is a counted no-op") &&
         expect(harness.decoded.stats().decodes == 1, "all nodes share one decode") &&
         expect(harness.textures->stats().uploads == 2,
                "one upload per (content, extent): 64px and 32px variants") &&
         ok;

    const std::vector<std::uint8_t> repeat =
        harness.render(scene.root, scene.styles, TextureFormat::R8G8B8A8Unorm, *harness.sink);
    ok = expect(first == repeat, "image paint is byte-identical on repeat") &&
         expect(harness.textures->stats().uploads == 2, "the repeat frame is a cache hit") && ok;

    RenderHarness srgb;
    if (srgb.init(TextureFormat::R8G8B8A8Srgb)) {
        Scene srgb_scene;
        srgb_scene.add_image(2, static_cast<float>(kNodeX), static_cast<float>(kNodeY),
                             static_cast<float>(kNodeSize), "images/checker.png");
        const std::vector<std::uint8_t> encoded = srgb.render(
            srgb_scene.root, srgb_scene.styles, TextureFormat::R8G8B8A8Srgb, *srgb.sink);
        ok = expect(pixel_near(encoded, kNodeX + 16, kNodeY + 16, {255, 0, 0, 255}, 2),
                    "the sRGB target round-trips the authored image bytes") &&
             ok;
    } else {
        std::cout << "paint_image_gpu: sRGB case skipped\n";
    }
    return ok;
}

void measure_nodes() {
    RenderHarness harness;
    if (!harness.init(TextureFormat::R8G8B8A8Unorm)) {
        std::cout << "paint_image_gpu: measure skipped\n";
        return;
    }
    for (const std::uint32_t nodes : {1U, 10U, 100U}) {
        image::ImageDecodeCache decoded;
        image::ImageTextureCache textures(*harness.device);
        tessera::ui::paint::ImagePaintConfig config;
        config.bundle = &harness.bundle;
        config.decoded = &decoded;
        config.textures = &textures;
        Batched2DSink sink(*harness.renderer, nullptr, nullptr,
                           tessera::ui::paint::VectorPaintConfig{}, config);
        Scene scene;
        for (std::uint32_t index = 0; index < nodes; ++index) {
            const float x = static_cast<float>(8 + (index % 4) * 36);
            const float y = static_cast<float>(8 + (index / 4) * 36);
            scene.add_image(2 + index, x, y, 32.0f, "images/checker.png");
        }
        const auto cold_start = std::chrono::steady_clock::now();
        (void)harness.render(scene.root, scene.styles, TextureFormat::R8G8B8A8Unorm, sink);
        const auto cold_end = std::chrono::steady_clock::now();
        constexpr int kWarmIterations = 10;
        double warm_paint_ms = 0.0;
        for (int iteration = 0; iteration < kWarmIterations; ++iteration) {
            const auto paint_begin = std::chrono::steady_clock::now();
            (void)harness.render(scene.root, scene.styles, TextureFormat::R8G8B8A8Unorm, sink);
            const auto paint_end = std::chrono::steady_clock::now();
            warm_paint_ms +=
                std::chrono::duration<double, std::milli>(paint_end - paint_begin).count();
        }
        const auto decode_stats = decoded.stats();
        const auto texture_stats = textures.stats();
        std::cout << std::fixed << std::setprecision(3) << "paint_image_measure: nodes=" << nodes
                  << " cold_frame_ms="
                  << std::chrono::duration<double, std::milli>(cold_end - cold_start).count()
                  << " warm_frame_ms=" << (warm_paint_ms / kWarmIterations)
                  << " decodes=" << decode_stats.decodes << " uploads=" << texture_stats.uploads
                  << " texture_hits=" << texture_stats.hits << "\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    const bool measure = argc > 1 && std::string_view(argv[1]) == "--measure";
    if (measure) {
        measure_nodes();
        return 0;
    }
    if (!vulkan_image_readback()) {
        std::cerr << "paint_image_gpu: FAILED\n";
        return 1;
    }
    std::cout << "paint_image_gpu: ok\n";
    return 0;
}
