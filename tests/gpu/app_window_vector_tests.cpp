// SPDX-License-Identifier: MIT
//
// The SVG stack on the real window path (ADR-071, #683): a hidden window with a Vulkan swapchain is
// given an embedded asset bundle through WindowOptions, and a scene node paints that asset. Every
// frame asks for a raster of a new size, so each frame is a miss that routes to the GPU rasterizer
// (the sizes are past the 128 px line), is flushed ahead of the draw list and is sampled by the
// frame that follows - the whole protocol of detail::VectorStack with two frames in flight.
//
// Asserts that vectors were drawn, that no GPU request fell back to ThorVG, and (through the
// validation layer, with synchronization validation on) that the run is validation-clean. Skips
// cleanly without a window backend or the validation layer; in a build with no SVG backend it
// asserts the documented no-op.

#include "tessera/app/window.hpp"
#include "tessera/rendering/path/svg_loader.hpp"
#include "tessera/rendering/vulkan/vulkan_validation.hpp"
#include "tessera/ui/assets/asset_bundle.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>

#ifndef TESSERA_TEST_FONT
#error "TESSERA_TEST_FONT must point at the repository test font"
#endif

namespace {

using tessera::app::WindowHost;
using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::PaintKind;
using tessera::ui::assets::AssetBundle;
using tessera::ui::assets::AssetEntry;
using tessera::ui::style::compile_stylesheet;

constexpr std::string_view kSvg =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64\" height=\"64\" viewBox=\"0 0 64 64\">"
    "<circle cx=\"32\" cy=\"32\" r=\"24\" fill=\"#e03030\"/>"
    "<path d=\"M12 40 L32 8 L52 40\" fill=\"none\" stroke=\"#2040d0\" stroke-width=\"4\"/></svg>";
constexpr std::uint32_t kFrames = 24;
constexpr std::uint32_t kMaxIterations = 400;
// Past the 128 px route line, and a different size every frame so each frame is a new raster.
constexpr std::uint32_t kFirstSize = 140;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_window_vector: " << message << "\n";
    }
    return condition;
}

void set_vector(Node& node, std::uint32_t size) {
    PaintData data;
    data.kind = PaintKind::Vector;
    data.vector.asset = "icon.svg";
    data.vector.raster_width = size;
    data.vector.raster_height = size;
    node.set_paint_data(std::move(data));
}

} // namespace

int main() {
    const std::byte* const bytes = reinterpret_cast<const std::byte*>(kSvg.data());
    const std::array<AssetEntry, 1> entries{
        AssetEntry{"icon.svg", 0, static_cast<std::uint32_t>(kSvg.size()), "image/svg+xml"}};
    const AssetBundle bundle(entries, std::span<const std::byte>(bytes, kSvg.size()));

    Node root(1);
    Node& icon = root.add_child(std::make_unique<Node>(2));
    icon.set_element_kind("View");
    set_vector(icon, kFirstSize);

    tessera::app::WindowOptions options;
    options.title = "Tessera vector";
    options.width = 320;
    options.height = 240;
    options.visible = false;
    options.font_path = TESSERA_TEST_FONT;
    options.validation = true;
    options.vector_assets = &bundle;

    std::uint64_t draws = 0;
    std::uint64_t gpu_rasters = 0;
    std::uint64_t gpu_fallbacks = 0;
    std::uint32_t presented = 0;
    bool stalled = false;
    {
        WindowHost host(root, compile_stylesheet("View { width: 200px; height: 200px; }"), options);
        if (!host.available()) {
            std::cout << "app_window_vector: skipped (no window backend or presentation surface)\n";
            return 0;
        }
        if (!tessera::rendering::vulkan::validation_active()) {
            std::cout << "app_window_vector: skipped (validation layer unavailable)\n";
            return 0;
        }
        std::uint32_t size = kFirstSize;
        for (std::uint32_t attempt = 0;
             attempt < kMaxIterations && host.stats().presented < kFrames; ++attempt) {
            const std::uint32_t before = host.stats().presented;
            set_vector(icon, ++size);
            (void)host.pump_once();
            if (host.stats().presented == before) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        const auto& stats = host.stats();
        draws = stats.vector_draws;
        gpu_rasters = stats.vector_gpu_rasterizations;
        gpu_fallbacks = stats.vector_gpu_fallbacks;
        presented = stats.presented;
        stalled = stats.stalled;
    }

    bool ok = true;
    ok = expect(presented >= kFrames, "the window presented the frames") && ok;
    ok = expect(!stalled, "no frame failed") && ok;
    if (!tessera::rendering::path::svg_backend_available()) {
        ok = expect(draws == 0, "without an SVG backend a vector draw is a no-op") && ok;
        return ok ? 0 : 1;
    }
    ok = expect(draws >= kFrames, "every presented frame drew the vector") && ok;
    ok = expect(gpu_fallbacks == 0, "no GPU raster fell back to ThorVG") && ok;
    std::cout << "app_window_vector: " << draws << " draws, " << gpu_rasters << " GPU rasters, "
              << gpu_fallbacks << " fallbacks over " << presented << " frames\n";
    return ok ? 0 : 1;
}
