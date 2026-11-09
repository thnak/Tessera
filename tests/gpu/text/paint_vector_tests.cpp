// SPDX-License-Identifier: MIT
//
// Real-Vulkan vector paint readback: an embedded SVG is set as PaintData on scene nodes, painted by
// paint_scene -> Batched2DSink (SvgDocumentCache + acquire_svg_texture) -> Batched2DRenderer, and
// read back. Asserts asset pixels inside each node's content box, background outside,
// byte-identical repeat, one shared document/texture upload for N nodes, missing-asset no-op, and
// the sRGB target path with validation enabled. Skips cleanly without a Vulkan device; in a
// ThorVG-OFF Vulkan build it asserts the documented no-op and returns.

#include "paint_vector_gpu_tests_assets.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/path/path_rasterizer.hpp"
#include "tessera/rendering/path/path_texture_cache.hpp"
#include "tessera/rendering/path/svg_document_cache.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/ui/assets/asset_svg.hpp"
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

namespace assets = tessera::ui::assets;
namespace path = tessera::rendering::path;
namespace fixtures = tessera::ui::paint::fixtures;

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
using tessera::ui::paint::VectorPaintConfig;
using tessera::ui::style::ComputedStyle;

constexpr Extent2D kExtent{160, 160};
constexpr std::uint32_t kNodeX = 8;
constexpr std::uint32_t kNodeY = 8;
constexpr std::uint32_t kNodeSize = 64;

[[nodiscard]] bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_vector_gpu: " << message << "\n";
    }
    return condition;
}

class StubStyles final : public StyleProvider {
public:
    void set(NodeId id, ComputedStyle style) {
        styles_[id] = std::move(style);
    }
    [[nodiscard]] ComputedStyle compute(const Node& node) override {
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? ComputedStyle{} : found->second;
    }

private:
    std::map<NodeId, ComputedStyle> styles_;
};

[[nodiscard]] ComputedStyle
styled(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle style;
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

void set_vector_node(Node& node, std::string_view asset) {
    PaintData data;
    data.kind = PaintKind::Vector;
    data.vector.asset = asset;
    node.set_paint_data(std::move(data));
}

struct Scene {
    Node root{1};
    StubStyles styles;

    Scene() {
        root.set_bounds(
            {0.0f, 0.0f, static_cast<float>(kExtent.width), static_cast<float>(kExtent.height)});
        styles.set(1, styled({{"background-color", "black"}}));
    }

    Node& add_asset(std::uint32_t id, float x, float y, std::string_view asset) {
        Node& node = root.add_child(std::make_unique<Node>(id));
        node.set_bounds({x, y, static_cast<float>(kNodeSize), static_cast<float>(kNodeSize)});
        set_vector_node(node, asset);
        return node;
    }

    void add_missing(std::uint32_t id, float x, float y) {
        add_child_missing(id, x, y);
    }

    void add_child_missing(std::uint32_t id, float x, float y) {
        Node& node = root.add_child(std::make_unique<Node>(id));
        node.set_bounds({x, y, static_cast<float>(kNodeSize), static_cast<float>(kNodeSize)});
        set_vector_node(node, "icons/missing.svg");
    }
};

struct RenderHarness {
    assets::AssetBundle bundle{fixtures::bundle()};
    std::unique_ptr<Device> device;
    std::unique_ptr<path::PathRasterizer> rasterizer;
    path::SvgDocumentCache documents;
    std::unique_ptr<path::PathTextureCache> textures;
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
            std::cout << "paint_vector_gpu: skipped (Vulkan device unavailable: " << error.what()
                      << ")\n";
            return false;
        }
        rasterizer = path::make_path_rasterizer(path::PathBackend::thorvg);
        if (rasterizer != nullptr) {
            path::PathTextureCacheConfig texture_config;
            texture_config.format = format;
            textures =
                std::make_unique<path::PathTextureCache>(*device, *rasterizer, texture_config);
        }
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
        VectorPaintConfig config;
        config.bundle = &bundle;
        config.documents = &documents;
        config.textures = textures.get();
        config.device_scale = 1.0f;
        config.texture_format = format;
        sink = std::make_unique<Batched2DSink>(*renderer, nullptr, nullptr, config);
        return true;
    }

    [[nodiscard]] bool software_path() const {
        return rasterizer != nullptr;
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

bool vulkan_paint_readback() {
    RenderHarness harness;
    if (!harness.init(TextureFormat::R8G8B8A8Unorm)) {
        return true; // no device: skip
    }
    Scene scene;
    scene.add_asset(2, static_cast<float>(kNodeX), static_cast<float>(kNodeY),
                    "icons/icon_shapes.svg");
    scene.add_asset(3, 88.0f, static_cast<float>(kNodeY), "icons/icon_shapes.svg");
    scene.add_missing(4, static_cast<float>(kNodeX), 88.0f);

    const bool software = harness.software_path();
    const std::vector<std::uint8_t> first =
        harness.render(scene.root, scene.styles, TextureFormat::R8G8B8A8Unorm, *harness.sink);
    if (!software) {
        // ThorVG OFF: documented no-op, no crash, background-only output.
        bool ok = expect(region_is_black(first, 0, 0, kExtent.width, kExtent.height),
                         "ThorVG OFF paints no vector pixels");
        const auto& stats = harness.sink->vector_stats();
        ok = expect(stats.draws == 0 && stats.unavailable_backend + stats.missing_resources == 3,
                    "ThorVG OFF counts each vector draw as a documented no-op") &&
             ok;
        std::cout << "paint_vector_gpu: ThorVG OFF, readback skipped\n";
        return ok;
    }

    std::cout << "paint_vector_gpu: Vulkan device available\n";

    // First node content box (8,8)-(72,72): icon_shapes has a #3366cc rect at doc (8..32) and a
    // translucent #cc6633 circle at doc (24..56). Second node repeats the same asset (cache hit).
    bool ok = expect(pixel_near(first, 8 + 16, 8 + 16, {51, 102, 204, 255}, 2),
                     "the opaque rect pixel is painted inside the content box");
    ok = expect(pixel_near(first, 8 + 40, 8 + 40, {102, 51, 26, 255}, 3),
                "the translucent circle pixel folds fill-opacity over the background") &&
         ok;
    ok = expect(pixel_near(first, 88 + 16, 8 + 16, {51, 102, 204, 255}, 2),
                "a second node sharing the asset paints the same pixels") &&
         ok;
    ok = expect(pixel_near(first, 120, 120, {0, 0, 0, 255}, 0),
                "pixels outside every content box stay background") &&
         ok;
    ok = expect(region_is_black(first, kNodeX, 88, kNodeX + kNodeSize, 88 + kNodeSize),
                "a missing asset paints nothing") &&
         ok;
    ok = expect(harness.sink->vector_stats().draws == 2, "two valid nodes emit two vector draws") &&
         expect(harness.sink->vector_stats().missing_assets == 1,
                "the missing asset is a counted no-op") &&
         ok;
    ok = expect(harness.documents.stats().decodes == 1,
                "both nodes share one document decode (content hash)") &&
         expect(harness.textures->stats().uploads == 1,
                "both nodes share one uploaded document texture") &&
         ok;

    const std::vector<std::uint8_t> repeat =
        harness.render(scene.root, scene.styles, TextureFormat::R8G8B8A8Unorm, *harness.sink);
    ok = expect(first == repeat, "vector paint is byte-identical on repeat") &&
         expect(harness.textures->stats().uploads == 1, "the repeat frame is a cache hit") && ok;

    // sRGB target: the cache format is sRGB so sampling decodes and the attachment re-encodes,
    // keeping the stored encoded bytes (D2 round-trip).
    RenderHarness srgb;
    if (srgb.init(TextureFormat::R8G8B8A8Srgb)) {
        Scene srgb_scene;
        srgb_scene.add_asset(2, static_cast<float>(kNodeX), static_cast<float>(kNodeY),
                             "icons/icon_shapes.svg");
        const std::vector<std::uint8_t> encoded = srgb.render(
            srgb_scene.root, srgb_scene.styles, TextureFormat::R8G8B8A8Srgb, *srgb.sink);
        ok = expect(pixel_near(encoded, 8 + 16, 8 + 16, {51, 102, 204, 255}, 2),
                    "the sRGB target round-trips the encoded asset bytes") &&
             ok;
    } else {
        std::cout << "paint_vector_gpu: sRGB case skipped\n";
    }
    return ok;
}

void measure_nodes() {
    RenderHarness harness;
    if (!harness.init(TextureFormat::R8G8B8A8Unorm) || !harness.software_path()) {
        std::cout << "paint_vector_gpu: measure skipped\n";
        return;
    }
    for (const std::uint32_t nodes : {1U, 10U, 100U}) {
        path::SvgDocumentCache documents;
        path::PathTextureCache textures(*harness.device, *harness.rasterizer, {});
        const assets::AssetBundle bundle = fixtures::bundle();
        VectorPaintConfig config;
        config.bundle = &bundle;
        config.documents = &documents;
        config.textures = &textures;
        Batched2DSink sink(*harness.renderer, nullptr, nullptr, config);
        Scene scene;
        for (std::uint32_t index = 0; index < nodes; ++index) {
            const float x = static_cast<float>(8 + (index % 4) * 36);
            const float y = static_cast<float>(8 + (index / 4) * 36);
            scene.add_asset(2 + index, x, y, "icons/icon_shapes.svg");
        }
        const auto cold_start = std::chrono::steady_clock::now();
        (void)harness.render(scene.root, scene.styles, TextureFormat::R8G8B8A8Unorm, sink);
        const auto cold_end = std::chrono::steady_clock::now();

        // Warm frames: measure the full frame and the paint pass alone (cache hits happen inside
        // paint_scene -> draw_vector); readback/present-style waits are excluded from paint_ms.
        constexpr int kWarmIterations = 10;
        double warm_total_ms = 0.0;
        double paint_total_ms = 0.0;
        const auto desc = target_desc(TextureFormat::R8G8B8A8Unorm);
        auto target = harness.device->create_render_target(desc);
        for (int iteration = 0; iteration < kWarmIterations; ++iteration) {
            const auto begin = std::chrono::steady_clock::now();
            auto commands = harness.device->create_command_list();
            harness.renderer->begin(*commands, *target, desc);
            const auto paint_begin = std::chrono::steady_clock::now();
            paint_scene(scene.root, scene.styles, sink);
            const auto paint_end = std::chrono::steady_clock::now();
            harness.renderer->end();
            commands->submit();
            harness.device->wait_idle();
            const auto end = std::chrono::steady_clock::now();
            warm_total_ms += std::chrono::duration<double, std::milli>(end - begin).count();
            paint_total_ms +=
                std::chrono::duration<double, std::milli>(paint_end - paint_begin).count();
        }
        std::cout << std::fixed << std::setprecision(3) << "paint_vector_measure: nodes=" << nodes
                  << " cold_frame_ms="
                  << std::chrono::duration<double, std::milli>(cold_end - cold_start).count()
                  << " warm_paint_ms=" << (paint_total_ms / kWarmIterations)
                  << " warm_frame_ms=" << (warm_total_ms / kWarmIterations)
                  << " decodes=" << documents.stats().decodes
                  << " uploads=" << textures.stats().uploads
                  << " texture_hits=" << textures.stats().hits << "\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    const bool measure = argc > 1 && std::string_view(argv[1]) == "--measure";
    if (measure) {
        measure_nodes();
        return 0;
    }
    if (!vulkan_paint_readback()) {
        std::cerr << "paint_vector_gpu: FAILED\n";
        return 1;
    }
    std::cout << "paint_vector_gpu: ok\n";
    return 0;
}
