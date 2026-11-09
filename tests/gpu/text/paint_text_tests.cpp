// SPDX-License-Identifier: MIT
//
// Real-Vulkan paint-text readback: a node's PaintData text is painted through
// paint_scene -> Batched2DSink -> TextRenderer/glyph atlas -> Batched2DRenderer, clipped by the
// paint clip stack. Asserts glyph coverage inside the text box, untouched background outside it,
// deterministic repeat, and that an sRGB target renders through the same path.
// Skips cleanly without a Vulkan device or the test font fixture.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/text_measure.hpp"
#include "tessera/ui/text/text_renderer.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::rhi::BufferUsage;
using tessera::rendering::rhi::Device;
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
using tessera::ui::style::ComputedStyle;
using tessera::ui::text::FontFace;
using tessera::ui::text::GlyphRasterizer;
using tessera::ui::text::HarfBuzzShaper;
using tessera::ui::text::TextConstraints;
using tessera::ui::text::TextLayout;
using tessera::ui::text::TextLine;
using tessera::ui::text::TextRenderer;
using tessera::ui::text::TextStyle;

constexpr Extent2D kExtent{160, 160};
constexpr std::uint32_t kTextRectX = 8;
constexpr std::uint32_t kTextRectY = 8;
constexpr std::uint32_t kTextRectW = 112;
constexpr std::uint32_t kTextRectH = 40;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_text_gpu: " << message << "\n";
    }
    return condition;
}

class StubStyles final : public StyleProvider {
public:
    void set(NodeId id, ComputedStyle style) {
        styles_[id] = std::move(style);
    }
    ComputedStyle compute(const Node& node) override {
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? ComputedStyle{} : found->second;
    }

private:
    std::map<NodeId, ComputedStyle> styles_;
};

ComputedStyle styled(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle style;
    for (const auto& [property, value] : values) {
        style.set(property, value);
    }
    return style;
}

struct RecordedLine {
    std::string text;
    tessera::rendering::rhi::Rect rect;
    std::optional<float> baseline;
};

class LineRecordingSink final : public tessera::ui::paint::PaintSink {
public:
    void fill_rect(const tessera::rendering::rhi::Rect&,
                   const tessera::rendering::rhi::Color&) override {}
    void fill_rounded_rect(const tessera::rendering::rhi::Rect&,
                           const tessera::rendering::rhi::Color&, float) override {}
    void push_clip(const tessera::ui::paint::RoundedClip&) override {}
    void pop_clip() override {}
    void draw_text(const tessera::ui::paint::TextDraw& run) override {
        lines.push_back(RecordedLine{std::string(run.text), run.rect, run.baseline});
    }
    std::vector<RecordedLine> lines;
};

RenderTargetDesc target_desc(TextureFormat format) {
    RenderTargetDesc desc;
    desc.extent = kExtent;
    desc.format = format;
    desc.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    desc.load_op = LoadOp::Clear;
    desc.store_op = StoreOp::Store;
    return desc;
}

std::vector<std::uint8_t> read_target(Device& device, RenderTarget& target) {
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

std::size_t count_non_background(const std::vector<std::uint8_t>& pixels) {
    std::size_t count = 0;
    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
        if (pixels[i] != 0 || pixels[i + 1] != 0 || pixels[i + 2] != 0) {
            ++count;
        }
    }
    return count;
}

std::size_t count_non_background_region(const std::vector<std::uint8_t>& pixels, std::uint32_t x0,
                                        std::uint32_t y0, std::uint32_t x1, std::uint32_t y1) {
    std::size_t count = 0;
    for (std::uint32_t y = y0; y < y1; ++y) {
        for (std::uint32_t x = x0; x < x1; ++x) {
            const std::size_t index =
                (static_cast<std::size_t>(y) * kExtent.width + x) * sizeof(std::uint32_t);
            if (pixels[index] != 0 || pixels[index + 1] != 0 || pixels[index + 2] != 0) {
                ++count;
            }
        }
    }
    return count;
}

std::size_t count_far_right_non_background(const std::vector<std::uint8_t>& pixels) {
    std::size_t count = 0;
    for (std::uint32_t y = 0; y < kExtent.height; ++y) {
        for (std::uint32_t x = kExtent.width - 4; x < kExtent.width; ++x) {
            const std::size_t index =
                (static_cast<std::size_t>(y) * kExtent.width + x) * sizeof(std::uint32_t);
            if (pixels[index] != 0 || pixels[index + 1] != 0 || pixels[index + 2] != 0) {
                ++count;
            }
        }
    }
    return count;
}

} // namespace

int main(int argc, char** argv) {
    using tessera::rendering::rhi::Backend;
    using tessera::rendering::rhi::DeviceDesc;

    if (argc < 2) {
        std::cout << "paint_text_gpu: skipped (missing font fixture path)\n";
        return 0;
    }
    const auto loaded = FontFace::load_from_file(argv[1]);
    if (!loaded) {
        std::cout << "paint_text_gpu: skipped (cannot load fixture: " << loaded.error().message
                  << ")\n";
        return 0;
    }

    DeviceDesc device_desc;
    device_desc.backend = Backend::Vulkan;
    device_desc.validation = true;

    std::unique_ptr<Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "paint_text_gpu: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }

    GlyphRasterizer rasterizer(*loaded);
    HarfBuzzShaper shaper(*loaded);
    TextRenderer text_renderer(*device, rasterizer, {128, 128, 2, 1});

    auto pipeline = device->create_pipeline(
        tessera::rendering::vulkan::batched2d_pipeline_desc(TextureFormat::R8G8B8A8Unorm, 1));

    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device->create_texture(white_desc);

    Batched2DRenderer renderer(*device, *pipeline, *white, Batched2DConfig{4096, false, true});
    Batched2DSink sink(renderer, &text_renderer, &shaper);

    auto pipeline_srgb = device->create_pipeline(
        tessera::rendering::vulkan::batched2d_pipeline_desc(TextureFormat::R8G8B8A8Srgb, 1));
    Batched2DRenderer renderer_srgb(*device, *pipeline_srgb, *white,
                                    Batched2DConfig{4096, false, true});
    Batched2DSink sink_srgb(renderer_srgb, &text_renderer, &shaper);

    StubStyles styles;
    styles.set(1, styled({{"background-color", "black"}, {"color", "white"}}));
    Node root(1);
    root.set_bounds({static_cast<float>(kTextRectX), static_cast<float>(kTextRectY),
                     static_cast<float>(kTextRectW), static_cast<float>(kTextRectH)});
    PaintData data;
    data.text = "Tessera";
    data.text_style.font_size = 24.0f;
    root.set_paint_data(std::move(data));

    Node wrapped_root(2);
    wrapped_root.set_bounds({8, 8, 96, 120});
    PaintData wrapped_data;
    wrapped_data.text = "The quick brown fox jumps over the lazy dog near the river bank at dawn";
    wrapped_data.wrap = tessera::ui::PaintWrap::Wrap;
    wrapped_data.text_style.font_size = 16.0f;
    wrapped_root.set_paint_data(std::move(wrapped_data));
    styles.set(2, styled({{"background-color", "black"},
                          {"color", "white"},
                          {"font-size", "16px"},
                          {"padding-left", "4px"},
                          {"padding-right", "4px"},
                          {"padding-top", "4px"},
                          {"padding-bottom", "4px"},
                          {"overflow-x", "hidden"},
                          {"overflow-y", "hidden"}}));

    const auto render_frame = [&](Batched2DRenderer& frame_renderer, Batched2DSink& frame_sink,
                                  Node& scene, RenderTarget& target, const RenderTargetDesc& desc,
                                  const tessera::ui::paint::PaintContext& context = {}) {
        auto commands = device->create_command_list();
        frame_renderer.begin(*commands, target, desc);
        frame_sink.begin_text_frame();
        paint_scene(scene, styles, frame_sink, context);
        frame_renderer.end();
        commands->submit();
        device->wait_idle();
        return read_target(*device, target);
    };

    const RenderTargetDesc desc1 = target_desc(TextureFormat::R8G8B8A8Unorm);
    auto target1 = device->create_render_target(desc1);
    const std::vector<std::uint8_t> first = render_frame(renderer, sink, root, *target1, desc1);
    const std::size_t coverage = count_non_background(first);
    bool ok = expect(coverage > 0, "text paints pixels inside its rect") &&
              expect(count_far_right_non_background(first) == 0,
                     "pixels outside the text rect stay background");

    const std::vector<std::uint8_t> repeat = render_frame(renderer, sink, root, *target1, desc1);
    ok = expect(first == repeat, "paint-text render is byte-identical on repeat") && ok;

    // sRGB target path: same paint path, colors encoded by the target format.
    const RenderTargetDesc desc_srgb = target_desc(TextureFormat::R8G8B8A8Srgb);
    auto target_srgb = device->create_render_target(desc_srgb);
    const std::vector<std::uint8_t> srgb =
        render_frame(renderer_srgb, sink_srgb, root, *target_srgb, desc_srgb);
    ok = expect(count_non_background(srgb) > 0, "text paints on an sRGB target") && ok;

    // Wrapped paragraph: multiple line bands inside the padded content box, background outside it,
    // deterministic repeat, and the same path on an sRGB target.
    const std::vector<std::uint8_t> wrapped =
        render_frame(renderer, sink, wrapped_root, *target1, desc1);
    const std::size_t first_band = count_non_background_region(wrapped, 12, 12, 100, 32);
    const std::size_t second_band = count_non_background_region(wrapped, 12, 32, 100, 52);
    const std::size_t third_band = count_non_background_region(wrapped, 12, 52, 100, 72);
    ok = expect(first_band > 0 && second_band > 0 && third_band > 0,
                "wrapped paragraph paints several line bands") &&
         ok;
    ok = expect(count_far_right_non_background(wrapped) == 0,
                "wrapped text stays inside its content box") &&
         ok;
    ok = expect(count_non_background_region(wrapped, 0, 140, kExtent.width, kExtent.height) == 0,
                "wrapped text stays above the node bottom") &&
         ok;

    const std::vector<std::uint8_t> wrapped_repeat =
        render_frame(renderer, sink, wrapped_root, *target1, desc1);
    ok = expect(wrapped == wrapped_repeat, "wrapped paragraph is byte-identical on repeat") && ok;

    const std::vector<std::uint8_t> wrapped_srgb =
        render_frame(renderer_srgb, sink_srgb, wrapped_root, *target_srgb, desc_srgb);
    ok = expect(count_non_background(wrapped_srgb) > 0,
                "wrapped paragraph paints on an sRGB target") &&
         ok;

    // Real-metrics source: line boxes use FreeType metrics + HarfBuzz advances (the same advances
    // the glyph renderer draws), so paint must emit exactly those boxes and lines must not overflow
    // the content box. The source id keeps real entries apart from stub entries in the cache.
    tessera::ui::paint::FontPaintTextMetrics real_metrics(*loaded, 42);
    TextStyle real_style;
    real_style.font_size = 16.0f;
    TextConstraints real_constraints;
    real_constraints.wrap = true;
    real_constraints.max_width = 88.0f; // wrapped_root: 96 wide minus 4+4 padding
    const std::string paragraph =
        "The quick brown fox jumps over the lazy dog near the river bank at dawn";
    const TextLayout real_layout = real_metrics.layout(paragraph, real_style, real_constraints);
    ok = expect(real_layout.lines.size() > 1, "real metrics wrap to multiple lines") && ok;
    for (const TextLine& line : real_layout.lines) {
        ok = expect(line.width <= real_constraints.max_width + 0.01f,
                    "real-metrics line never overflows the content width") &&
             ok;
    }

    tessera::ui::paint::TextLayoutCache real_cache;
    tessera::ui::paint::PaintContext real_context;
    real_context.text_metrics_source = &real_metrics;
    real_context.text_layout_cache = &real_cache;

    LineRecordingSink line_sink;
    paint_scene(wrapped_root, styles, line_sink, real_context);
    const auto cache_stats = real_cache.stats();
    (void)cache_stats;
    ok = expect(line_sink.lines.size() == real_layout.lines.size(),
                "paint emits one run per real-metrics line box") &&
         ok;
    LineRecordingSink line_sink_repeat;
    paint_scene(wrapped_root, styles, line_sink_repeat, real_context);
    const auto cache_stats_after = real_cache.stats();
    ok = expect(cache_stats_after.hits == 1 && cache_stats_after.misses == 1 &&
                    cache_stats_after.entries == 1,
                "real-metrics paint misses once then hits") &&
         ok;
    float line_top = 12.0f; // bounds y 8 + padding-top 4
    for (std::size_t index = 0; index < real_layout.lines.size() && index < line_sink.lines.size();
         ++index) {
        const TextLine& line = real_layout.lines[index];
        const RecordedLine& recorded = line_sink.lines[index];
        ok = expect(recorded.text == paragraph.substr(line.begin, line.end - line.begin),
                    "paint line slice matches the real-metrics line range") &&
             ok;
        ok = expect(recorded.rect.y == std::round(line_top),
                    "paint line top matches the snapped line-box stack") &&
             ok;
        ok = expect(recorded.baseline.value_or(-1.0f) == std::round(line_top + line.baseline),
                    "paint baseline matches the line-box baseline") &&
             ok;
        line_top += line.height;
    }

    // Center alignment stays inside the content box with real metrics.
    Node centered(3);
    centered.set_bounds({8, 8, 96, 120});
    PaintData centered_data;
    centered_data.text = paragraph;
    centered_data.wrap = tessera::ui::PaintWrap::Wrap;
    centered_data.text_align = tessera::ui::PaintTextAlign::Center;
    centered_data.text_style.font_size = 16.0f;
    centered.set_paint_data(std::move(centered_data));
    styles.set(3, styled({{"background-color", "black"},
                          {"color", "white"},
                          {"font-size", "16px"},
                          {"padding-left", "4px"},
                          {"padding-right", "4px"},
                          {"padding-top", "4px"},
                          {"padding-bottom", "4px"},
                          {"overflow-x", "hidden"},
                          {"overflow-y", "hidden"}}));
    LineRecordingSink centered_sink;
    paint_scene(centered, styles, centered_sink, real_context);
    ok = expect(!centered_sink.lines.empty(), "centered real-metrics paint emits lines") && ok;
    for (const RecordedLine& recorded : centered_sink.lines) {
        const float expected_x = std::round(12.0f + (88.0f - recorded.rect.width) * 0.5f);
        ok = expect(recorded.rect.x == expected_x && recorded.rect.x >= 12.0f &&
                        recorded.rect.x + recorded.rect.width <= 100.0f,
                    "real-metrics center alignment stays inside the content box") &&
             ok;
    }

    // GPU readback with the real-metrics source: several line bands, nothing outside the content
    // box, deterministic repeat and an sRGB target.
    const std::vector<std::uint8_t> real_wrapped =
        render_frame(renderer, sink, wrapped_root, *target1, desc1, real_context);
    ok = expect(count_non_background_region(real_wrapped, 12, 12, 100, 32) > 0 &&
                    count_non_background_region(real_wrapped, 12, 32, 100, 52) > 0,
                "real-metrics wrapped lines paint inside the content box") &&
         ok;
    ok = expect(count_far_right_non_background(real_wrapped) == 0 &&
                    count_non_background_region(real_wrapped, 0, 140, kExtent.width,
                                                kExtent.height) == 0,
                "real-metrics wrapped text stays inside its content box") &&
         ok;
    const std::vector<std::uint8_t> real_wrapped_repeat =
        render_frame(renderer, sink, wrapped_root, *target1, desc1, real_context);
    ok = expect(real_wrapped == real_wrapped_repeat,
                "real-metrics wrapped paint is byte-identical on repeat") &&
         ok;
    const std::vector<std::uint8_t> real_wrapped_srgb =
        render_frame(renderer_srgb, sink_srgb, wrapped_root, *target_srgb, desc_srgb, real_context);
    ok = expect(count_non_background(real_wrapped_srgb) > 0,
                "real-metrics wrapped paint works on an sRGB target") &&
         ok;

    // Stub and real metrics sources never mix in one cache: the same text+geometry under the two
    // source ids produces two entries (and different line breaks), and neither hits the other.
    tessera::ui::paint::TextLayoutCache mixed_cache;
    tessera::ui::paint::PaintContext stub_context;
    stub_context.text_layout_cache = &mixed_cache;
    LineRecordingSink stub_sink;
    paint_scene(wrapped_root, styles, stub_sink, stub_context);
    tessera::ui::paint::PaintContext mixed_real_context;
    mixed_real_context.text_layout_cache = &mixed_cache;
    mixed_real_context.text_metrics_source = &real_metrics;
    LineRecordingSink mixed_real_sink;
    paint_scene(wrapped_root, styles, mixed_real_sink, mixed_real_context);
    const auto mixed_stats = mixed_cache.stats();
    ok = expect(mixed_stats.misses == 2 && mixed_stats.entries == 2 && mixed_stats.hits == 0,
                "stub and real metrics ids never mix in the cache") &&
         ok;
    ok = expect(!stub_sink.lines.empty() && !mixed_real_sink.lines.empty() &&
                    (stub_sink.lines[0].text != mixed_real_sink.lines[0].text ||
                     stub_sink.lines[0].rect.width != mixed_real_sink.lines[0].rect.width),
                "real metrics select a different wrap than the stub") &&
         ok;

    if (argc > 2 && std::string_view(argv[2]) == "--bench") {
        std::string words;
        for (int word = 0; word < 200; ++word) {
            if (word > 0) {
                words += ' ';
            }
            words += "word" + std::to_string(word);
        }
        TextStyle bench_style;
        bench_style.font_size = 16.0f;
        TextConstraints bench_constraints;
        bench_constraints.wrap = true;
        bench_constraints.max_width = 300.0f;
        const tessera::ui::paint::StubPaintTextMetrics stub_source;
        const auto time_source = [&](const tessera::ui::paint::PaintTextMetricsSource& source) {
            const auto started = std::chrono::steady_clock::now();
            for (int iteration = 0; iteration < 100; ++iteration) {
                (void)source.layout(words, bench_style, bench_constraints);
            }
            const auto elapsed = std::chrono::steady_clock::now() - started;
            return static_cast<double>(
                       std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) /
                   (100.0 * 1000.0);
        };
        const double stub_us = time_source(stub_source);
        const double real_us = time_source(real_metrics);
        const std::size_t bench_lines =
            real_metrics.layout(words, bench_style, bench_constraints).lines.size();
        tessera::ui::paint::TextLayoutCache bench_cache;
        const auto& ignored =
            bench_cache.get(words, bench_style, bench_constraints, real_metrics, 0);
        (void)ignored;
        const auto hit_started = std::chrono::steady_clock::now();
        for (int iteration = 0; iteration < 200; ++iteration) {
            (void)bench_cache.get(words, bench_style, bench_constraints, real_metrics, 0);
        }
        const auto hit_elapsed = std::chrono::steady_clock::now() - hit_started;
        const double hit_us =
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(hit_elapsed).count()) /
            (200.0 * 1000.0);
        std::cout << "paint_text_gpu bench: 200 words, 300px -> " << bench_lines
                  << " lines, stub=" << stub_us << " us, real=" << real_us
                  << " us, real cache hit=" << hit_us << " us (Debug)\n";
    }

    if (!ok) {
        std::cerr << "paint_text_gpu: failed\n";
        return 1;
    }
    std::cout << "paint_text_gpu: ok (coverage=" << coverage << ")\n";
    return 0;
}
