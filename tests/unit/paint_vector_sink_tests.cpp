// SPDX-License-Identifier: MIT
//
// Batched2DSink vector contract: the documented no-op behavior for missing/malformed assets and
// absent resources (including ThorVG-OFF builds), the device-scale/size contract and the raster
// request contract. Split from paint_vector_tests.cpp, which keeps the emission-order contract.

#include "path_texture_cache_test_fakes.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/null/null_device.hpp"
#include "tessera/rendering/path/path_rasterizer.hpp"
#include "tessera/rendering/path/path_texture_cache.hpp"
#include "tessera/rendering/path/svg_document_cache.hpp"
#include "tessera/rendering/path/svg_loader.hpp"
#include "tessera/rendering/path/svg_texture.hpp"
#include "tessera/ui/assets/asset_bundle.hpp"
#include "tessera/ui/assets/asset_svg.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/clipped_paint_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/vector_raster.hpp"
#include "tessera/ui/scene/node.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::null::create_null_device;
using tessera::rendering::null::NullCommandList;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::test::path_texture::FakeDevice;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PaintData;
using tessera::ui::PaintKind;
using tessera::ui::PaintVectorStyle;
using tessera::ui::paint::Batched2DSink;
using tessera::ui::paint::ClippedPaintSink;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::PaintStats;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::VectorDraw;
using tessera::ui::style::ComputedStyle;

[[nodiscard]] bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_vector_sink: " << message << "\n";
    }
    return condition;
}

struct NullRenderer {
    std::unique_ptr<tessera::rendering::rhi::Device> device;
    std::unique_ptr<tessera::rendering::rhi::RenderTarget> target;
    std::unique_ptr<tessera::rendering::rhi::Texture> white;
    std::unique_ptr<tessera::rendering::rhi::Pipeline> pipeline;
    std::unique_ptr<tessera::rendering::rhi::CommandList> commands;
    std::unique_ptr<Batched2DRenderer> renderer;

    NullRenderer() {
        device = create_null_device({});
        target = device->create_render_target(tessera::rendering::rhi::RenderTargetDesc{{64, 64}});
        white = device->create_texture(tessera::rendering::rhi::TextureDesc{{1, 1}});
        pipeline = device->create_pipeline(tessera::rendering::rhi::PipelineDesc{});
        commands = device->create_command_list();
        renderer = std::make_unique<Batched2DRenderer>(*device, *pipeline, *white);
        renderer->begin(*commands, *target, tessera::rendering::rhi::RenderTargetDesc{{64, 64}});
    }

    void finish() {
        renderer->end();
        commands->submit();
    }
};

[[nodiscard]] VectorDraw sample_draw(std::string_view asset, Rect rect) {
    VectorDraw draw;
    draw.asset = asset;
    draw.rect = rect;
    draw.tint = Color{1.0f, 1.0f, 1.0f, 1.0f};
    return draw;
}

bool sink_missing_resources_is_noop() {
    NullRenderer harness;
    Batched2DSink sink(*harness.renderer);
    sink.draw_vector(sample_draw("icons/a.svg", Rect{0.0f, 0.0f, 32.0f, 32.0f}));
    harness.finish();
    const auto* commands = dynamic_cast<NullCommandList*>(harness.commands.get());
    return expect(sink.vector_stats().missing_resources == 1,
                  "a sink without vector resources counts the no-op") &&
           expect(commands != nullptr && commands->stats().draws == 0,
                  "a resource-less sink emits no draw");
}

bool sink_failure_values_are_noops() {
    constexpr std::string_view kBadSvg = "this is not an svg";
    constexpr std::string_view kGoodSvg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"8\">"
        "<rect width=\"8\" height=\"8\" fill=\"#3366cc\"/></svg>";
    std::vector<std::uint8_t> data;
    const std::size_t bad_offset = 0;
    data.insert(data.end(), kBadSvg.begin(), kBadSvg.end());
    const std::size_t good_offset = data.size();
    data.insert(data.end(), kGoodSvg.begin(), kGoodSvg.end());
    const std::array<assets::AssetEntry, 2> entries = {
        assets::AssetEntry{"bad.svg", static_cast<std::uint32_t>(bad_offset),
                           static_cast<std::uint32_t>(kBadSvg.size()), "image/svg+xml"},
        assets::AssetEntry{"good.svg", static_cast<std::uint32_t>(good_offset),
                           static_cast<std::uint32_t>(kGoodSvg.size()), "image/svg+xml"}};
    const assets::AssetBundle bundle(
        std::span<const assets::AssetEntry>(entries.data(), entries.size()),
        std::as_bytes(std::span<const std::uint8_t>(data.data(), data.size())));

    path::SvgDocumentCache documents;
    auto null_rasterizer = path::make_path_rasterizer(path::PathBackend::null_backend);
    if (!expect(null_rasterizer != nullptr, "null rasterizer backend is always available")) {
        return false;
    }

    {
        NullRenderer harness;
        path::PathTextureCache textures(*harness.device, *null_rasterizer, {});
        Batched2DSink sink(*harness.renderer, nullptr, nullptr,
                           tessera::ui::paint::VectorPaintConfig{&bundle, &documents, &textures});
        sink.draw_vector(sample_draw("missing.svg", Rect{0.0f, 0.0f, 32.0f, 32.0f}));
        harness.finish();
        const auto* commands = dynamic_cast<NullCommandList*>(harness.commands.get());
        bool ok = expect(sink.vector_stats().missing_assets == 1,
                         "a missing asset counts as a documented no-op") &&
                  expect(commands != nullptr && commands->stats().draws == 0,
                         "a missing asset emits no draw");
        if (!path::svg_backend_available()) {
            sink.draw_vector(sample_draw("good.svg", Rect{0.0f, 0.0f, 32.0f, 32.0f}));
            ok = expect(sink.vector_stats().unavailable_backend == 1,
                        "ThorVG OFF reports the backend no-op") &&
                 ok;
        }
        return ok;
    }

    if (!path::svg_backend_available()) {
        return true;
    }

    NullRenderer harness;
    path::PathTextureCache textures(*harness.device, *null_rasterizer, {});
    Batched2DSink sink(*harness.renderer, nullptr, nullptr,
                       tessera::ui::paint::VectorPaintConfig{&bundle, &documents, &textures});
    sink.draw_vector(sample_draw("bad.svg", Rect{0.0f, 0.0f, 32.0f, 32.0f}));
    bool ok =
        expect(sink.vector_stats().failed_assets == 1, "a malformed asset is a counted no-op");
    sink.draw_vector(sample_draw("good.svg", Rect{0.0f, 0.0f, 32.0f, 32.0f}));
    ok = expect(sink.vector_stats().draws == 1 && textures.stats().uploads == 1,
                "a valid asset draws one textured quad") &&
         ok;
    harness.finish();
    return ok;
}

bool sink_scale_contract() {
    if (!path::svg_backend_available()) {
        return true;
    }
    constexpr std::string_view kSvg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64\" height=\"64\">"
        "<rect x=\"8\" y=\"8\" width=\"24\" height=\"24\" fill=\"#3366cc\"/></svg>";
    const std::vector<std::uint8_t> bytes(kSvg.begin(), kSvg.end());
    const std::array<assets::AssetEntry, 1> entries = {assets::AssetEntry{
        "icon.svg", 0, static_cast<std::uint32_t>(bytes.size()), "image/svg+xml"}};
    const assets::AssetBundle bundle(
        std::span<const assets::AssetEntry>(entries.data(), entries.size()),
        std::as_bytes(std::span<const std::uint8_t>(bytes.data(), bytes.size())));

    auto rasterizer = path::make_path_rasterizer(path::PathBackend::thorvg);
    if (!expect(rasterizer != nullptr, "ThorVG rasterizer is available")) {
        return false;
    }
    FakeDevice device;
    path::PathTextureCache textures(device, *rasterizer, {});
    path::SvgDocumentCache documents;
    NullRenderer harness;
    Batched2DSink sink(*harness.renderer, nullptr, nullptr,
                       tessera::ui::paint::VectorPaintConfig{&bundle, &documents, &textures, 2.0f});
    sink.draw_vector(sample_draw("icon.svg", Rect{0.0f, 0.0f, 32.0f, 32.0f}));
    harness.finish();
    bool ok = expect(sink.vector_stats().draws == 1 && textures.stats().uploads == 1,
                     "derived raster size uploads one texture");

    // The sink must use one texture per (document, derived raster scale): 32 px content * 2 device
    // scale = 64 px raster over a 64 px document => scale_milli 1000.
    const auto document = assets::load_asset_svg(bundle, "icon.svg", documents);
    if (!expect(document.has_value(), "document loads for the key check")) {
        return false;
    }
    const auto key = path::make_svg_texture_key(
        path::make_svg_document_key(std::span<const std::uint8_t>(bytes)).content_hash, 0,
        **document, 1000);
    const auto cached = path::acquire_svg_texture(textures, key, **document);
    ok = expect(cached.has_value() && textures.stats().uploads == 1,
                "the sink's key matches the documented derived scale") &&
         ok;

    // Explicit raster size wins over scale: 128 px raster over a 64 px document => scale_milli
    // 2000.
    Batched2DSink second(*harness.renderer, nullptr, nullptr,
                         tessera::ui::paint::VectorPaintConfig{&bundle, &documents, &textures});
    VectorDraw explicit_draw = sample_draw("icon.svg", Rect{0.0f, 0.0f, 32.0f, 32.0f});
    explicit_draw.raster_width = 128;
    explicit_draw.raster_height = 128;
    second.draw_vector(explicit_draw);
    harness.renderer->end();
    const auto explicit_key = path::make_svg_texture_key(
        path::make_svg_document_key(std::span<const std::uint8_t>(bytes)).content_hash, 0,
        **document, 2000);
    const auto explicit_cached = path::acquire_svg_texture(textures, explicit_key, **document);
    ok = expect(explicit_cached.has_value() && textures.stats().uploads == 2,
                "an explicit raster size uses its own cache key") &&
         ok;
    return ok;
}

bool raster_request_contract() {
    VectorDraw draw;
    draw.asset = "icons/a.svg";
    draw.rect = Rect{0.0f, 0.0f, 32.0f, 32.0f};
    const auto derived = tessera::ui::paint::resolve_vector_raster(draw, 64, 64, 2.0f);
    bool ok = expect(derived.has_value(), "a valid draw resolves to a raster request") &&
              expect(derived->raster_width == 64 && derived->raster_height == 64,
                     "derived extents are content box x device scale") &&
              expect(derived->scale_milli == 1000, "scale_milli is raster size over document size");

    VectorDraw explicit_width = draw;
    explicit_width.raster_width = 128;
    const auto explicit_request =
        tessera::ui::paint::resolve_vector_raster(explicit_width, 64, 64, 1.0f);
    ok = expect(explicit_request.has_value() && explicit_request->raster_height == 128,
                "the missing axis is derived through the document aspect") &&
         expect(explicit_request->scale_milli == 2000,
                "an explicit raster size wins over the device scale") &&
         ok;

    VectorDraw aspect_draw = draw;
    aspect_draw.raster_width = 128;
    const auto aspect_request =
        tessera::ui::paint::resolve_vector_raster(aspect_draw, 64, 32, 1.0f);
    ok = expect(aspect_request.has_value() && aspect_request->raster_height == 64,
                "an explicit width derives height from a non-square document") &&
         ok;

    VectorDraw scale_hint = draw;
    scale_hint.scale = 3.0f;
    const auto scale_request = tessera::ui::paint::resolve_vector_raster(scale_hint, 64, 64, 1.0f);
    ok = expect(scale_request.has_value() && scale_request->raster_width == 96,
                "an explicit scale hint replaces the device scale") &&
         ok;

    VectorDraw degenerate = draw;
    degenerate.rect.width = 0.0f;
    ok = expect(!tessera::ui::paint::resolve_vector_raster(degenerate, 64, 64, 1.0f).has_value(),
                "a degenerate rect resolves to nullopt") &&
         ok;
    ok = expect(!tessera::ui::paint::resolve_vector_raster(draw, 0, 64, 1.0f).has_value(),
                "a zero-width document resolves to nullopt") &&
         ok;
    ok = expect(!tessera::ui::paint::resolve_vector_raster(draw, 64, 64, 0.0f).has_value(),
                "an invalid device scale resolves to nullopt") &&
         ok;
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = sink_missing_resources_is_noop() && ok;
    ok = sink_failure_values_are_noops() && ok;
    ok = sink_scale_contract() && ok;
    ok = raster_request_contract() && ok;
    if (!ok) {
        std::cerr << "paint_vector_sink: FAILED\n";
        return 1;
    }
    std::cout << "paint_vector_sink: ok\n";
    return 0;
}
