// SPDX-License-Identifier: MIT
//
// Manual measurement tool (not a CTest): the number issue #480 part 2 is gated on.
//
// `Batched2DRenderer` ends a batch whenever the sampled texture changes, so images and glyph-atlas
// pages interleaved with text cost one draw call per switch. Part 2 (texture arrays via descriptor
// indexing) would remove exactly that share. The issue says to measure it first and close part 2
// with the numbers if the per-frame switch count is low, so this tool reports:
//
//  - the batch-split attribution of a realistic interleaved scene (text + images + solid fills),
//  - the same scene's command-recording time, and
//  - the draw-call count with and without the texture share, i.e. what part 2 could buy.
//
// It runs on the null RHI (deterministic counters, no GPU needed) and, when a Vulkan device is
// available, repeats the recording on the real backend so the time is the driver's, not the null
// backend's. Nothing here is a pass/fail gate: the numbers go into the issue and the docs.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/image/image_decode_cache.hpp"
#include "tessera/rendering/image/image_texture_cache.hpp"
#include "tessera/rendering/null/null_device.hpp"
#include "tessera/ui/assets/asset_bundle.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/text_renderer.hpp"
#include "texture_batch_measure_assets.hpp"

#if defined(TESSERA_TEST_HAS_VULKAN)
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
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

namespace gpu = tessera::rendering::gpu;
namespace image = tessera::rendering::image;
namespace nullrhi = tessera::rendering::null;
namespace rhi = tessera::rendering::rhi;
namespace text = tessera::ui::text;
namespace ui = tessera::ui;

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kWidth = 640;
constexpr std::uint32_t kHeight = 480;
// Rows of "image, caption, image, caption" - the shape the issue describes (images interleaved with
// text). 24 rows is a dense dashboard, not a synthetic worst case.
constexpr std::uint32_t kRows = 24;

[[nodiscard]] double milliseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

class StubStyles final : public ui::paint::StyleProvider {
public:
    void set(ui::NodeId id, ui::style::ComputedStyle style) {
        styles_[id] = std::move(style);
    }
    ui::style::ComputedStyle compute(const ui::Node& node) override {
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? ui::style::ComputedStyle{} : found->second;
    }

private:
    std::map<ui::NodeId, ui::style::ComputedStyle> styles_;
};

ui::style::ComputedStyle styled(std::initializer_list<std::pair<const char*, const char*>> values) {
    ui::style::ComputedStyle style;
    for (const auto& [property, value] : values) {
        style.set(property, value);
    }
    return style;
}

// The interleaved scene: per row an image, a caption, a second image and a body line, plus a solid
// row background. Every image shares one asset (one texture), so the switches are image <-> glyph
// page, which is the case descriptor indexing would merge.
//
// `images_as_fills` swaps each image quad for a solid fill of the same rect: the quad count, the
// vertex count and the paint traversal are identical, and only the texture identity changes. That
// control isolates the draw-call cost of the texture switches from every other difference.
void build_scene(ui::Node& root, StubStyles& styles, bool images_as_fills) {
    root.set_bounds({0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)});
    styles.set(1, styled({{"background-color", "#0B1220"}}));

    ui::NodeId next_id = 2;
    const float row_height = static_cast<float>(kHeight) / static_cast<float>(kRows);
    for (std::uint32_t row = 0; row < kRows; ++row) {
        const float y = static_cast<float>(row) * row_height;
        auto& background = root.add_child(std::make_unique<ui::Node>(next_id++));
        background.set_bounds({0, y, static_cast<float>(kWidth), row_height});
        styles.set(background.id(),
                   styled({{"background-color", row % 2U == 0U ? "#111827" : "#0F172A"}}));

        const auto add_image = [&](float x, float w) {
            auto& node = root.add_child(std::make_unique<ui::Node>(next_id++));
            node.set_bounds({x, y + 2.0f, w, row_height - 4.0f});
            if (!images_as_fills) {
                ui::PaintData data;
                data.kind = ui::PaintKind::Image;
                data.image.asset = "images/checker.png";
                node.set_paint_data(std::move(data));
            }
            styles.set(node.id(), styled({{"background-color", "#1E293B"}}));
        };
        const auto add_text = [&](float x, float w, std::string value, const char* size) {
            auto& node = root.add_child(std::make_unique<ui::Node>(next_id++));
            node.set_bounds({x, y + 2.0f, w, row_height - 4.0f});
            ui::PaintData data;
            data.kind = ui::PaintKind::Text;
            data.text = std::move(value);
            node.set_paint_data(std::move(data));
            styles.set(node.id(), styled({{"color", "#E2E8F0"}, {"font-size", size}}));
        };

        add_image(8.0f, row_height - 4.0f);
        add_text(8.0f + row_height, 180.0f, "Row " + std::to_string(row) + " caption", "13px");
        add_image(8.0f + row_height + 190.0f, row_height - 4.0f);
        add_text(8.0f + 2.0f * row_height + 190.0f, 240.0f,
                 "Deterministic body copy for row " + std::to_string(row), "12px");
    }
}

struct Outcome {
    std::uint32_t draw_calls = 0;
    std::uint32_t vertices = 0;
    gpu::BatchSplitStats split;
    double record_ms = 0.0;
};

// One frame through the production paint path: paint_scene -> Batched2DSink -> Batched2DRenderer.
Outcome paint_frame(rhi::Device& device, rhi::RenderTarget& target, rhi::Pipeline& pipeline,
                    rhi::Texture& white, ui::Node& root, StubStyles& styles,
                    text::TextRenderer* text_renderer, text::TextShaper* shaper,
                    image::ImageDecodeCache& decoded, image::ImageTextureCache& textures,
                    const ui::assets::AssetBundle& bundle) {
    auto commands = device.create_command_list();
    gpu::Batched2DRenderer renderer(device, pipeline, white);
    ui::paint::Batched2DSink sink(renderer, text_renderer, shaper, {},
                                  ui::paint::ImagePaintConfig{&bundle, &decoded, &textures, 1.0f});

    const rhi::RenderTargetDesc desc{{kWidth, kHeight}};
    const Clock::time_point begin = Clock::now();
    renderer.begin(*commands, target, desc);
    sink.begin_text_frame();
    (void)ui::paint::paint_scene(root, styles, sink);
    renderer.end();
    const Clock::time_point end = Clock::now();
    commands->submit();

    Outcome outcome;
    outcome.record_ms = milliseconds(begin, end);
    outcome.draw_calls = renderer.draw_calls();
    outcome.vertices = renderer.vertex_count();
    outcome.split = renderer.batch_split_stats();
    return outcome;
}

void report(const char* lane, const Outcome& outcome) {
    const gpu::BatchSplitStats& split = outcome.split;
    const std::uint32_t structural = split.total() - split.texture;
    std::cout << std::fixed << std::setprecision(3) << lane << ": draw_calls=" << outcome.draw_calls
              << " vertices=" << outcome.vertices << " record_ms=" << outcome.record_ms
              << " splits{texture=" << split.texture << " pipeline=" << split.pipeline
              << " scissor=" << split.scissor << " clip=" << split.clip << " flush=" << split.flush
              << "}" << " without_texture_share=" << structural << "\n";
}

// One arm of the measurement: the interleaved scene, or its same-geometry control with the images
// painted as solid fills. Both arms are built once and painted twice (the first frame uploads the
// image and rasterizes the glyphs; the second is the steady state a scrolling screen pays).
// `Node` is non-copyable, so an arm is owned through a pointer.
struct Arm {
    ui::Node root{1};
    StubStyles styles;
};

std::unique_ptr<Arm> make_arm(bool images_as_fills) {
    auto arm = std::make_unique<Arm>();
    build_scene(arm->root, arm->styles, images_as_fills);
    return arm;
}

// Paints `arm` twice on `device` and returns the steady-state outcome.
Outcome measure_arm(rhi::Device& device, rhi::RenderTarget& target, rhi::Pipeline& pipeline,
                    rhi::Texture& white, Arm& arm, text::GlyphRasterizer& rasterizer,
                    text::TextShaper& shaper, image::ImageDecodeCache& decoded,
                    image::ImageTextureCache& textures, const ui::assets::AssetBundle& bundle) {
    text::TextRenderer text_renderer(device, rasterizer, {512, 512, 4, 1});
    (void)paint_frame(device, target, pipeline, white, arm.root, arm.styles, &text_renderer,
                      &shaper, decoded, textures, bundle);
    textures.end_frame();
    return paint_frame(device, target, pipeline, white, arm.root, arm.styles, &text_renderer,
                       &shaper, decoded, textures, bundle);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: texture_batch_measure <tests/fixtures/fonts/NotoSans-Regular.ttf>\n";
        return 2;
    }
    const auto loaded = text::FontFace::load_from_file(argv[1]);
    if (!loaded) {
        std::cerr << "texture_batch_measure: cannot load the font fixture: "
                  << loaded.error().message << "\n";
        return 2;
    }
    text::FontFace face = std::move(*loaded);
    text::GlyphRasterizer rasterizer(face);
    text::HarfBuzzShaper shaper(face);

    const std::unique_ptr<Arm> interleaved = make_arm(false);
    const std::unique_ptr<Arm> control = make_arm(true);

    // The raster-image fixture bytes are embedded by the build (see CMakeLists.txt).
    const ui::assets::AssetBundle bundle = tessera::test::texture_batch_measure::fixtures::bundle();

    // Null RHI: deterministic counters, no GPU. This is the number the issue asks for.
    {
        auto device = nullrhi::create_null_device({});
        auto target = device->create_render_target({{kWidth, kHeight}});
        auto white = device->create_texture({{1, 1}});
        auto pipeline = device->create_pipeline({});
        image::ImageDecodeCache decoded;
        image::ImageTextureCache textures(*device);
        report("null/interleaved", measure_arm(*device, *target, *pipeline, *white, *interleaved,
                                               rasterizer, shaper, decoded, textures, bundle));
        report("null/control", measure_arm(*device, *target, *pipeline, *white, *control,
                                           rasterizer, shaper, decoded, textures, bundle));
    }

    // Real backend, when one is available: the same recording through the driver.
#if defined(TESSERA_TEST_HAS_VULKAN)
    try {
        rhi::DeviceDesc desc;
        desc.backend = rhi::Backend::Vulkan;
        auto device = tessera::rendering::vulkan::create_vulkan_device(desc);
        auto target = device->create_render_target({{kWidth, kHeight}});
        auto white = device->create_texture({{1, 1}});
        auto pipeline =
            device->create_pipeline(tessera::rendering::vulkan::batched2d_pipeline_desc());
        image::ImageDecodeCache decoded;
        image::ImageTextureCache textures(*device);
        report("vulkan/interleaved", measure_arm(*device, *target, *pipeline, *white, *interleaved,
                                                 rasterizer, shaper, decoded, textures, bundle));
        report("vulkan/control", measure_arm(*device, *target, *pipeline, *white, *control,
                                             rasterizer, shaper, decoded, textures, bundle));
    } catch (const std::exception& error) {
        std::cout << "vulkan: skipped (" << error.what() << ")\n";
    }
#else
    std::cout << "vulkan: skipped (built without the Vulkan backend)\n";
#endif
    return 0;
}
