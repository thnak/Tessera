// SPDX-License-Identifier: MIT

#if defined(_WIN32)
// The Vulkan test-hook headers pull in Windows headers; keep min/max out of std::min/std::max.
#define NOMINMAX
#endif

#include "cpu_paint_sink.hpp"
#include "damage_cli.hpp"
#include "tessera/diagnostics/metrics.hpp"
#include "tessera/diagnostics/overlay.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/hal/window_wait.hpp"
#include "tessera/rendering/golden/image_diff.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/null/null_device.hpp"
#include "tessera/rendering/rhi/swapchain.hpp"
#include "tessera/rendering/software/cpu_canvas.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/runtime/device_recovery.hpp"
#include "tessera/runtime/frame_pacer.hpp"
#include "tessera/runtime/frame_wait.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/runtime/steady_clock.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/input.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "text_demo.hpp"
#if defined(TESSERA_VULKAN_TEST_HOOKS)
#include "vulkan_internal.hpp"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <stb_image.h>
#include <stb_image_write.h>

namespace {

struct Options {
    std::string backend = "vulkan";
    std::string out = "batched2d.png";
    std::string raw;
    std::uint32_t width = 512;
    std::uint32_t height = 512;
    bool validation = false;
    bool golden = false;
    bool overlay = false;
    bool shapes = false;
    bool cpu_text = false;
    bool simulate_loss = false;
    std::uint32_t msaa = 1;
    bool windowed = false;
    bool on_demand = false;
    double run_seconds = 0.5;
    std::uint32_t frames = 3;
    std::string font;
    std::string text = "Tessera";
    float text_size = 24.0f;
    std::string compare;
    std::string report;
    std::string diff;
    std::uint32_t tolerance = 2;
    double max_mismatch = 0.005;
    std::vector<tessera::rendering::rhi::Rect> damage;
};

void print_usage() {
    std::cout << "usage: batched2d_poc [--backend vulkan|null] [--out path] [--raw path]\n"
                 "                     [--width N] [--height N] [--validation] [--golden]\n"
                 "                     [--damage x,y,w,h]... [--font path.ttf] [--text string]\n"
                 "                     [--text-size N] [--compare golden.png] [--tolerance N]\n"
                 "                     [--max-mismatch ratio] [--overlay] [--shapes] "
                 "[--cpu-text] [--msaa N] [--simulate-loss] [--report "
                 "report.json]\n"
                 "                     [--diff diff.png] [--windowed [--frames N]] [--on-demand "
                 "[--run-seconds S]]\n";
}

bool parse_options(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return false;
        }
        if (arg == "--backend" && i + 1 < argc) {
            options.backend = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            options.out = argv[++i];
        } else if (arg == "--raw" && i + 1 < argc) {
            options.raw = argv[++i];
        } else if (arg == "--width" && i + 1 < argc) {
            options.width = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--height" && i + 1 < argc) {
            options.height = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--validation") {
            options.validation = true;
        } else if (arg == "--golden") {
            options.golden = true;
        } else if (arg == "--overlay") {
            options.overlay = true;
        } else if (arg == "--shapes") {
            options.shapes = true;
        } else if (arg == "--cpu-text") {
            options.cpu_text = true;
        } else if (arg == "--msaa" && i + 1 < argc) {
            options.msaa = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--simulate-loss") {
            options.simulate_loss = true;
        } else if (arg == "--windowed") {
            options.windowed = true;
        } else if (arg == "--run-seconds" && i + 1 < argc) {
            options.run_seconds = std::stod(argv[++i]);
        } else if (arg == "--on-demand") {
            options.windowed = true;
            options.on_demand = true;
        } else if (arg == "--frames" && i + 1 < argc) {
            options.frames = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--font" && i + 1 < argc) {
            options.font = argv[++i];
        } else if (arg == "--text" && i + 1 < argc) {
            options.text = argv[++i];
        } else if (arg == "--text-size" && i + 1 < argc) {
            options.text_size = std::stof(argv[++i]);
        } else if (arg == "--compare" && i + 1 < argc) {
            options.compare = argv[++i];
        } else if (arg == "--tolerance" && i + 1 < argc) {
            options.tolerance = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--max-mismatch" && i + 1 < argc) {
            options.max_mismatch = std::stod(argv[++i]);
        } else if (arg == "--report" && i + 1 < argc) {
            options.report = argv[++i];
        } else if (arg == "--diff" && i + 1 < argc) {
            options.diff = argv[++i];
        } else if (arg == "--damage" && i + 1 < argc) {
            tessera::rendering::rhi::Rect rect;
            const char* value = argv[++i];
            if (!tessera::poc::batched2d::parse_damage_rect(value, rect)) {
                std::cerr << "invalid damage rect: " << value << " (expected x,y,w,h)\n";
                return false;
            }
            options.damage.push_back(rect);
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            print_usage();
            return false;
        }
    }
    return true;
}

void build_screen(tessera::rendering::gpu::Batched2DRenderer& renderer,
                  tessera::rendering::rhi::Texture& checker, std::uint32_t width,
                  std::uint32_t height) {
    using tessera::rendering::rhi::Mat3;
    using tessera::rendering::rhi::Rect;
    using tessera::rendering::rhi::rgba8;

    const auto background = rgba8(11, 18, 32);
    const auto panel = rgba8(15, 23, 42);
    const auto card = rgba8(30, 41, 59);
    const auto card_light = rgba8(51, 65, 85);
    const auto accent = rgba8(56, 189, 248);
    const auto green = rgba8(74, 222, 128);
    const auto yellow = rgba8(250, 204, 21);
    const auto red = rgba8(248, 113, 113);
    const auto text = rgba8(226, 232, 240);
    const auto muted = rgba8(148, 163, 184);

    renderer.push_rect({0, 0, static_cast<float>(width), static_cast<float>(height)}, background);
    renderer.push_rect({0, 0, static_cast<float>(width), 48}, panel);
    renderer.push_rect({0, 0, static_cast<float>(width), 3}, accent);
    renderer.push_rect({0, 48, 120, static_cast<float>(height) - 48}, panel);

    renderer.push_rounded_rect({12, 64, 96, 28}, accent, 8);
    renderer.push_rounded_rect({12, 100, 96, 28}, card, 8);
    renderer.push_rounded_rect({12, 136, 96, 28}, card, 8);

    renderer.push_rounded_rect({140, 64, 160, 104}, card, 12);
    renderer.push_rounded_rect({152, 76, 40, 40}, accent, 20);
    renderer.push_rounded_rect({204, 82, 80, 8}, text, 4);
    renderer.push_rounded_rect({204, 98, 56, 6}, muted, 3);
    renderer.push_rounded_rect({152, 128, 136, 6}, card_light, 3);
    renderer.push_rounded_rect({152, 128, 92, 6}, green, 3);

    renderer.push_rounded_rect({316, 64, 180, 104}, card, 12);
    const float bar_heights[6] = {24, 40, 32, 56, 44, 64};
    const std::array<tessera::rendering::rhi::Color, 6> bar_colors = {accent, green,  accent,
                                                                      yellow, accent, red};
    for (int i = 0; i < 6; ++i) {
        const float bar_height = bar_heights[i];
        renderer.push_rounded_rect(
            {328.0f + static_cast<float>(i) * 26.0f, 152.0f - bar_height, 16.0f, bar_height},
            bar_colors[i], 4);
    }

    renderer.push_rounded_rect({140, 180, 356, 100}, card, 12);
    for (int i = 1; i < 5; ++i) {
        renderer.push_rect({152, 180.0f + static_cast<float>(i) * 16.0f, 332, 1}, card_light);
    }
    renderer.push_rounded_rect({152, 238, 64, 8}, accent, 4);
    renderer.push_rounded_rect({224, 220, 80, 26}, green, 6);
    renderer.push_rounded_rect({312, 228, 48, 18}, yellow, 6);
    renderer.push_rounded_rect({368, 208, 100, 38}, red, 8);

    renderer.push_rounded_rect({140, 296, 356, 184}, card, 12);
    renderer.push_scissor({150, 306, 336, 164});
    for (int row = 0; row < 5; ++row) {
        for (int col = 0; col < 8; ++col) {
            const auto color = ((row + col) % 3 == 0) ? accent : card_light;
            renderer.push_rounded_rect({152.0f + static_cast<float>(col) * 42.0f,
                                        308.0f + static_cast<float>(row) * 32.0f, 36, 26},
                                       color, 6);
        }
    }
    renderer.push_texture({414, 320, 64, 64}, checker, {0, 0, 4, 4}, rgba8(255, 255, 255));
    renderer.pop_scissor();

    renderer.push_transform(Mat3::translation(180, 400));
    renderer.push_rounded_rect({0, 0, 120, 32}, panel, 8);
    renderer.pop_transform();

    renderer.push_rounded_rect({320, 400, 156, 32}, panel, 8);
    renderer.push_rounded_rect({330, 410, 72, 12}, accent, 6);
    renderer.push_rounded_rect({410, 410, 40, 12}, green, 6);
}

void build_shapes(tessera::rendering::gpu::Batched2DRenderer& renderer,
                  tessera::rendering::rhi::Texture& checker, std::uint32_t width,
                  std::uint32_t height) {
    using tessera::rendering::rhi::Mat3;
    using tessera::rendering::rhi::rgba8;

    const auto background = rgba8(11, 18, 32);
    const auto panel = rgba8(15, 23, 42);
    const auto card = rgba8(30, 41, 59);
    const auto accent = rgba8(56, 189, 248);
    const auto green = rgba8(74, 222, 128);
    const auto yellow = rgba8(250, 204, 21);
    const auto red = rgba8(248, 113, 113);
    const auto text = rgba8(226, 232, 240);
    const auto muted = rgba8(148, 163, 184);
    const auto white = rgba8(255, 255, 255);

    renderer.push_rect({0, 0, static_cast<float>(width), static_cast<float>(height)}, background);

    // Rounded cards and a large-radius accent circle.
    renderer.push_rounded_rect({40, 40, 180, 140}, card, 28);
    renderer.push_rounded_rect({64, 64, 64, 64}, accent, 32);
    renderer.push_rounded_rect({148, 64, 48, 12}, text, 6);
    renderer.push_rounded_rect({148, 84, 80, 8}, muted, 4);

    // Textured quads at two UV scales.
    renderer.push_rounded_rect({240, 40, 232, 140}, card, 16);
    renderer.push_texture({256, 56, 72, 72}, checker, {0, 0, 4, 4}, white);
    renderer.push_texture({344, 56, 112, 108}, checker, {0, 0, 8, 8}, white);

    // Scissor-clipped content.
    renderer.push_scissor({40, 210, 200, 140});
    renderer.push_rect({20, 190, 260, 180}, panel);
    renderer.push_rounded_rect({60, 230, 160, 100}, green, 20);
    renderer.push_rounded_rect({100, 270, 80, 20}, yellow, 10);
    renderer.pop_scissor();

    // Translated rounded content.
    renderer.push_transform(Mat3::translation(280, 210));
    renderer.push_rounded_rect({0, 0, 200, 140}, red, 24);
    renderer.push_rounded_rect({20, 20, 80, 80}, accent, 20);
    renderer.pop_transform();

    // Clip + transform together, with several abutting rounded tiles.
    renderer.push_scissor({40, 380, 432, 90});
    renderer.push_rect({40, 380, 432, 90}, panel);
    renderer.push_transform(Mat3::translation(56, 396));
    const std::array<tessera::rendering::rhi::Color, 7> colors = {accent, green, yellow, red,
                                                                  accent, green, yellow};
    for (std::uint32_t i = 0; i < colors.size(); ++i) {
        renderer.push_rounded_rect({static_cast<float>(i) * 56.0f, 0, 44, 44}, colors[i], 10);
    }
    renderer.pop_transform();
    renderer.pop_scissor();
}

std::unique_ptr<tessera::poc::batched2d::TextDemo>
make_text_demo(tessera::rendering::rhi::Device& device, const Options& options) {
    if (options.font.empty()) {
        return nullptr;
    }
    auto face = tessera::ui::text::FontFace::load_from_file(options.font);
    if (!face) {
        throw std::runtime_error("failed to load font: " + face.error().message);
    }
    return std::make_unique<tessera::poc::batched2d::TextDemo>(device, std::move(*face),
                                                               options.text, options.text_size);
}

void draw_text_overlay(tessera::poc::batched2d::TextDemo* text,
                       tessera::rendering::gpu::Batched2DRenderer& renderer, bool overlay) {
    if (text == nullptr) {
        return;
    }
    text->begin_frame();
    text->draw(renderer, overlay ? 360.0f : 150.0f, 34.0f,
               tessera::rendering::rhi::rgba8(226, 232, 240));
    std::cout << "text glyphs=" << text->atlas().stats().misses + text->atlas().stats().hits
              << " pages=" << text->atlas().page_count() << "\n";
}

void draw_diagnostics_overlay(tessera::poc::batched2d::TextDemo* text,
                              tessera::rendering::gpu::Batched2DRenderer& renderer,
                              const Options& options) {
    if (!options.overlay) {
        return;
    }
    if (text == nullptr) {
        throw std::runtime_error("--overlay requires --font");
    }
    using tessera::diagnostics::MetricKind;
    using tessera::diagnostics::MetricSample;
    using tessera::diagnostics::OverlayFrame;
    using tessera::diagnostics::OverlayModel;

    // Capture the pre-overlay work without flushing: a mid-frame flush would upload the same
    // vertex buffer again and invalidate the scene draw that is still recording.
    OverlayFrame frame;
    frame.frame = 1;
    frame.steps = 1;
    frame.completions = 0;
    frame.damage_rects = options.damage.size();
    frame.scissors = options.damage.size();
    frame.full_redraw = options.damage.empty();

    const MetricSample metrics[] = {
        {"paint.batches", renderer.pending_batches(), MetricKind::Counter},
        {"paint.vertices", renderer.vertex_count(), MetricKind::Counter},
    };
    const auto lines = OverlayModel::build(frame, metrics, 2);
    renderer.push_rounded_rect({8, 8, 250, 88}, tessera::rendering::rhi::rgba8(15, 23, 42, 232), 8);
    text->draw_lines(renderer, lines, 16.0f, 26.0f, 18.0f, 14,
                     tessera::rendering::rhi::rgba8(226, 232, 240));
}

std::array<std::uint8_t, 8 * 8 * 4> make_checker_pixels() {
    std::array<std::uint8_t, 8 * 8 * 4> pixels{};
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const bool light = ((x / 2 + y / 2) % 2) == 0;
            const std::size_t index = static_cast<std::size_t>((y * 8 + x) * 4);
            pixels[index + 0] = light ? 56 : 30;
            pixels[index + 1] = light ? 189 : 41;
            pixels[index + 2] = light ? 248 : 59;
            pixels[index + 3] = 255;
        }
    }
    return pixels;
}

void write_raw(const std::string& path, const std::vector<std::uint8_t>& pixels) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("failed to open raw output: " + path);
    }
    stream.write(reinterpret_cast<const char*>(pixels.data()),
                 static_cast<std::streamsize>(pixels.size()));
}

void compare_with_golden(const Options& options, const std::vector<std::uint8_t>& pixels) {
    if (options.compare.empty()) {
        return;
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load(options.compare.c_str(), &width, &height, &channels, 4);
    if (decoded == nullptr) {
        throw std::runtime_error("failed to load golden image: " + options.compare);
    }
    std::vector<std::uint8_t> expected(decoded, decoded + static_cast<std::size_t>(width) *
                                                              static_cast<std::size_t>(height) * 4);
    stbi_image_free(decoded);

    const auto diff = tessera::rendering::golden::compare_rgba(
        {options.width, options.height}, pixels,
        {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)}, expected,
        options.tolerance);

    if (!options.report.empty()) {
        std::ofstream stream(options.report);
        if (!stream) {
            throw std::runtime_error("failed to open golden report: " + options.report);
        }
        stream << tessera::rendering::golden::golden_report_json(
                      diff, options.compare, options.tolerance, options.max_mismatch)
               << "\n";
    }

    if (!options.diff.empty() && !diff.size_mismatch) {
        std::vector<std::uint8_t> diff_pixels(expected.size());
        for (std::size_t pixel = 0; pixel < diff.pixels; ++pixel) {
            bool mismatched = false;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                const std::size_t index = pixel * 4 + channel;
                const auto a = static_cast<std::uint32_t>(pixels[index]);
                const auto b = static_cast<std::uint32_t>(expected[index]);
                const std::uint32_t delta = a > b ? a - b : b - a;
                mismatched = mismatched || delta > options.tolerance;
            }
            std::uint8_t value = 0;
            if (mismatched) {
                value = 255;
            } else {
                value = static_cast<std::uint8_t>(
                    (static_cast<std::uint32_t>(expected[pixel * 4]) +
                     static_cast<std::uint32_t>(expected[pixel * 4 + 1]) +
                     static_cast<std::uint32_t>(expected[pixel * 4 + 2])) /
                    3);
            }
            diff_pixels[pixel * 4] = mismatched ? 255 : value;
            diff_pixels[pixel * 4 + 1] = value;
            diff_pixels[pixel * 4 + 2] = mismatched ? 255 : value;
            diff_pixels[pixel * 4 + 3] = 255;
        }
        const int wrote =
            stbi_write_png(options.diff.c_str(), width, height, 4, diff_pixels.data(), width * 4);
        if (wrote == 0) {
            throw std::runtime_error("failed to write golden diff image: " + options.diff);
        }
    }

    std::cout << "golden: pixels=" << diff.pixels << " mismatched=" << diff.mismatched
              << " max_delta=" << diff.max_channel_delta
              << " mean_delta=" << diff.mean_channel_delta() << " ratio=" << diff.mismatch_ratio()
              << "\n";
    if (!tessera::rendering::golden::within_tolerance(diff, options.max_mismatch)) {
        throw std::runtime_error("golden comparison failed: " + options.compare);
    }
}

int run_vulkan(const Options& options) {
    using namespace tessera::rendering;

    auto device =
        vulkan::create_vulkan_device(rhi::DeviceDesc{rhi::Backend::Vulkan, options.validation, -1});

    rhi::RenderTargetDesc target_desc;
    target_desc.extent = {options.width, options.height};
    target_desc.clear_color = rhi::rgba8(11, 18, 32);

    auto target = device->create_render_target(target_desc);

    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device->create_texture(white_desc);

    auto checker_pixels = make_checker_pixels();
    rhi::TextureDesc checker_desc;
    checker_desc.extent = {8, 8};
    checker_desc.pixels = checker_pixels.data();
    checker_desc.row_pitch = 8 * 4;
    checker_desc.filter = rhi::Filter::Nearest;
    auto checker = device->create_texture(checker_desc);

    auto pipeline = device->create_pipeline(vulkan::batched2d_pipeline_desc());

    const std::size_t pixel_bytes = static_cast<std::size_t>(options.width) * options.height * 4;
    auto readback = device->create_buffer({pixel_bytes, rhi::BufferUsage::Readback});
    auto commands = device->create_command_list();

    gpu::Batched2DRenderer renderer(*device, *pipeline, *white,
                                    gpu::Batched2DConfig{4096, true, options.golden});
    auto text_demo = make_text_demo(*device, options);
    tessera::poc::batched2d::begin_with_damage(renderer, *commands, *target, target_desc,
                                               options.damage);
    if (options.shapes) {
        build_shapes(renderer, *checker, options.width, options.height);
    } else {
        build_screen(renderer, *checker, options.width, options.height);
    }
    draw_text_overlay(text_demo.get(), renderer, options.overlay);
    draw_diagnostics_overlay(text_demo.get(), renderer, options);
    renderer.end();

    commands->copy_target_to_buffer(*target, *readback);
    commands->submit();
    device->wait_idle();

    std::vector<std::uint8_t> pixels(pixel_bytes);
    readback->read(pixels.data(), pixels.size());

    const int ok = stbi_write_png(options.out.c_str(), static_cast<int>(options.width),
                                  static_cast<int>(options.height), 4, pixels.data(),
                                  static_cast<int>(options.width * 4));
    if (ok == 0) {
        throw std::runtime_error("failed to write PNG: " + options.out);
    }
    if (!options.raw.empty()) {
        write_raw(options.raw, pixels);
    }
    compare_with_golden(options, pixels);

    std::cout << "draw_calls=" << renderer.draw_calls() << " vertices=" << renderer.vertex_count()
              << "\n";
    std::cout << "wrote " << options.out << " (" << options.width << "x" << options.height << ")\n";
    return 0;
}

int run_null(const Options& options) {
    using namespace tessera::rendering;

    auto device = null::create_null_device(rhi::DeviceDesc{});
    rhi::RenderTargetDesc target_desc;
    target_desc.extent = {options.width, options.height};
    auto target = device->create_render_target(target_desc);

    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device->create_texture(white_desc);

    auto checker_pixels = make_checker_pixels();
    rhi::TextureDesc checker_desc;
    checker_desc.extent = {8, 8};
    checker_desc.pixels = checker_pixels.data();
    checker_desc.row_pitch = 8 * 4;
    auto checker = device->create_texture(checker_desc);

    auto pipeline = device->create_pipeline(rhi::PipelineDesc{});
    auto commands = device->create_command_list();

    gpu::Batched2DRenderer renderer(*device, *pipeline, *white,
                                    gpu::Batched2DConfig{4096, true, options.golden});
    auto text_demo = make_text_demo(*device, options);
    tessera::poc::batched2d::begin_with_damage(renderer, *commands, *target, target_desc,
                                               options.damage);
    if (options.shapes) {
        build_shapes(renderer, *checker, options.width, options.height);
    } else {
        build_screen(renderer, *checker, options.width, options.height);
    }
    draw_text_overlay(text_demo.get(), renderer, options.overlay);
    draw_diagnostics_overlay(text_demo.get(), renderer, options);
    renderer.end();
    commands->submit();

    auto* null_commands = dynamic_cast<null::NullCommandList*>(commands.get());
    std::cout << "draw_calls=" << renderer.draw_calls() << " vertices=" << renderer.vertex_count()
              << "\n";
    if (null_commands != nullptr) {
        const null::NullCommandStats& stats = null_commands->stats();
        std::cout << "null_draws=" << stats.draws << " null_vertices=" << stats.vertices << "\n";
    }
    return 0;
}

int run_windowed(const Options& options) {
    using namespace tessera;
    using namespace tessera::rendering;

    hal::WindowDesc window_desc;
    window_desc.title = "Tessera windowed POC";
    window_desc.width = options.width;
    window_desc.height = options.height;
    window_desc.visible = false;
    auto window = hal::create_window(window_desc);
    if (window == nullptr) {
        std::cout << "windowed: skipped (no platform window backend)\n";
        return 0;
    }
    const hal::NativeWindowHandle native = window->native_handle();
    if (native.window == nullptr) {
        std::cout << "windowed: skipped (window has no native handle)\n";
        return 0;
    }

    auto device =
        vulkan::create_vulkan_device(rhi::DeviceDesc{rhi::Backend::Vulkan, options.validation, -1});

    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device->create_texture(white_desc);

    auto checker_pixels = make_checker_pixels();
    rhi::TextureDesc checker_desc;
    checker_desc.extent = {8, 8};
    checker_desc.pixels = checker_pixels.data();
    checker_desc.row_pitch = 8 * 4;
    checker_desc.filter = rhi::Filter::Nearest;
    auto checker = device->create_texture(checker_desc);

    const hal::WindowMetrics initial = window->metrics();
    rhi::SwapchainDesc swap_desc;
    swap_desc.extent = {initial.physical_width, initial.physical_height};
    swap_desc.image_count = 2;
    swap_desc.vsync = true;
    swap_desc.native_window = native.window;
    swap_desc.native_display = native.display;

    auto created = device->create_swapchain(swap_desc);
    if (!created) {
        if (created.error() == rhi::SwapchainError::NotSupported) {
            std::cout << "windowed: skipped (presentation surface unavailable)\n";
            return 0;
        }
        throw std::runtime_error("windowed: create_swapchain failed");
    }
    std::unique_ptr<rhi::Swapchain> swapchain = std::move(*created);

    auto pipeline_desc = vulkan::batched2d_pipeline_desc();
    pipeline_desc.color_format = swapchain->format(); // follow the surface format the driver chose
    auto pipeline = device->create_pipeline(pipeline_desc);
    // Two frames may be on the GPU at once: the renderer rotates its vertex buffers over the same
    // number of slots, and the loop below only reuses a slot after that frame's list completed.
    constexpr std::size_t kFramesInFlight = 2;
    constexpr std::uint32_t kSlotWaitMs = 1000;
    gpu::Batched2DConfig renderer_config{4096, true, true};
    renderer_config.frames_in_flight = static_cast<std::uint32_t>(kFramesInFlight);
    gpu::Batched2DRenderer renderer(*device, *pipeline, *white, renderer_config);
    std::array<std::unique_ptr<rhi::CommandList>, kFramesInFlight> in_flight;
    std::size_t frame_slot = 0;

    runtime::FramePacer pacer(runtime::FramePacerConfig{60.0});
    pacer.present(hal::steady_now_seconds());
    runtime::SteadyClock clock;
    runtime::RuntimeLoop loop(clock);

    struct FrameOutcome {
        bool rendered = false;
        bool presented = false;
    };
    // Records one frame into the next swapchain image, queues it asynchronously and presents.
    // `paint` fills the renderer. Returns what actually happened so callers count real frames.
    auto render_present = [&](auto&& paint) -> FrameOutcome {
        FrameOutcome outcome;
        const hal::WindowMetrics metrics = window->metrics();
        if (metrics.physical_width != swapchain->extent().width ||
            metrics.physical_height != swapchain->extent().height) {
            swapchain->resize({metrics.physical_width, metrics.physical_height});
        }

        // The frame that last used this slot must be finished before its vertex buffer is
        // rewritten. The wait is bounded; a stuck GPU skips the frame instead of freezing the loop.
        if (in_flight[frame_slot] && !in_flight[frame_slot]->wait(kSlotWaitMs)) {
            return outcome;
        }
        in_flight[frame_slot].reset();

        std::optional<std::uint32_t> image;
        for (int attempt = 0; attempt < 4 && !image; ++attempt) {
            const auto acquired = swapchain->acquire_next_image(100);
            if (acquired) {
                image = *acquired;
                break;
            }
            if (acquired.error() == rhi::SwapchainError::OutOfDate) {
                swapchain->resize({metrics.physical_width, metrics.physical_height});
                continue;
            }
            break;
        }
        if (!image) {
            return outcome;
        }

        auto* target = swapchain->image_target(*image);
        if (target == nullptr) {
            return outcome;
        }

        rhi::RenderTargetDesc target_desc;
        target_desc.extent = swapchain->extent();
        target_desc.format = swapchain->format();
        target_desc.load_op = rhi::LoadOp::Clear;
        target_desc.clear_color = rhi::rgba8(11, 18, 32);

        auto commands = device->create_command_list();
        renderer.begin(*commands, *target, target_desc);
        paint(target_desc.extent.width, target_desc.extent.height);
        renderer.end();
        commands->submit_async();
        in_flight[frame_slot] = std::move(commands);
        frame_slot = (frame_slot + 1) % kFramesInFlight;
        outcome.rendered = true;

        const auto result = swapchain->present(*image);
        if (result || result.error() == rhi::SwapchainError::Suboptimal) {
            outcome.presented = true;
        } else if (result.error() == rhi::SwapchainError::OutOfDate) {
            swapchain->resize({metrics.physical_width, metrics.physical_height});
        }
        pacer.present(hal::steady_now_seconds());
        return outcome;
    };

    std::array<hal::WindowEvent, 16> events{};
    std::uint32_t rendered = 0;
    std::uint32_t presented = 0;

    if (options.on_demand) {
        // Scene-driven loop: a Node tree is the source of truth, FramePipeline turns dirty flags
        // into damage, and RuntimeLoop (render-on-demand) decides whether a frame is needed. With
        // nothing dirty the loop parks on the window wait and presents nothing. Swapchain images
        // are not preserved between frames, so every rendered frame is a full redraw; the damage
        // plan only decides whether to render at all.
        struct Item {
            ui::Node* node = nullptr;
            rhi::Color color{};
        };
        ui::Node root(1);
        std::array<Item, 3> items;
        const auto layout_scene = [&](float width, float height) {
            root.set_bounds({0, 0, width, height});
            items[0].node->set_bounds({0, 0, width, 48});
            items[1].node->set_bounds({24, 72, width * 0.5f, height * 0.4f});
            items[2].node->set_bounds({width - 96, height - 96, 72, 72});
        };
        for (std::size_t i = 0; i < items.size(); ++i) {
            auto node = std::make_unique<ui::Node>(static_cast<ui::NodeId>(2 + i));
            items[i].node = &root.add_child(std::move(node));
        }
        items[0].color = rhi::rgba8(30, 41, 59);
        items[1].color = rhi::rgba8(56, 189, 248);
        items[2].color = rhi::rgba8(74, 222, 128);
        layout_scene(static_cast<float>(swapchain->extent().width),
                     static_cast<float>(swapchain->extent().height));

        runtime::RuntimeLoopConfig loop_config;
        loop_config.render_on_demand = true;
        runtime::RuntimeLoop demand_loop(clock, loop_config);
        ui::animation::AnimationRuntime animations;
        rendering::DamageAccumulator damage(rendering::DamageConfig{swapchain->extent(), 8});
        ui::layout::DamageTracker tracker;
        ui::DirtyPass dirty_pass;
        ui::frame::FramePipeline pipeline(animations, demand_loop, tracker, damage, dirty_pass);

        // Real pointer input arrives through this queue: the window translates OS messages into
        // ui::InputEvent. A press recolors the card and a move drags the square, so each input
        // that changes the scene dirties a node and earns exactly one frame.
        ui::InputQueue input_queue(256);
        window->set_input_queue(&input_queue);
        std::uint32_t input_seen = 0;
        bool input_pending = false;
        double first_input_time = 0.0;
        std::vector<double> input_latency_ms;
        bool card_alt = false;

        // Scripted scene changes keep the smoke test deterministic without real input: each one
        // dirties a node, so the loop must render exactly once for it and then go idle again.
        const double run_seconds = options.run_seconds;
        constexpr std::array<double, 2> kScripted = {0.15, 0.30};
        const double start = hal::steady_now_seconds();
        std::size_t next_script = 0;
        bool first_frame = true;
        bool cause_pending = true; // the first frame is caused by startup
        std::uint32_t scripted_unrendered = 0;
        std::uint32_t scripted_fired = 0;
        std::uint32_t unjustified = 0;
        std::uint32_t idle_waits = 0;
        std::uint32_t events_seen = 0;
        double last_tick = start;
        bool closed = false;

        while (!closed) {
            const double now = hal::steady_now_seconds();
            if (now - start >= run_seconds) {
                break;
            }

            const std::size_t event_count = window->poll_events(events);
            for (std::size_t i = 0; i < event_count; ++i) {
                ++events_seen;
                cause_pending = true;
                switch (events[i].type) {
                case hal::WindowEventType::Close:
                    closed = true;
                    break;
                case hal::WindowEventType::Resize:
                case hal::WindowEventType::DpiChange: {
                    const hal::WindowMetrics metrics = window->metrics();
                    layout_scene(static_cast<float>(metrics.physical_width),
                                 static_cast<float>(metrics.physical_height));
                    damage = rendering::DamageAccumulator(rendering::DamageConfig{
                        rhi::Extent2D{metrics.physical_width, metrics.physical_height}, 8});
                    for (const Item& item : items) {
                        item.node->mark_dirty(ui::Dirty::Layout | ui::Dirty::Paint);
                    }
                    break;
                }
                case hal::WindowEventType::Focus:
                case hal::WindowEventType::Blur:
                    items[2].color = events[i].type == hal::WindowEventType::Focus
                                         ? rhi::rgba8(74, 222, 128)
                                         : rhi::rgba8(100, 116, 139);
                    items[2].node->mark_dirty(ui::Dirty::Paint);
                    break;
                default:
                    break;
                }
            }
            if (closed) {
                break;
            }

            ui::InputEvent input;
            while (input_queue.pop(input)) {
                ++input_seen;
                bool changed = false;
                if (input.type == ui::InputEventType::PointerDown) {
                    card_alt = !card_alt;
                    items[1].color = card_alt ? rhi::rgba8(251, 146, 60) : rhi::rgba8(56, 189, 248);
                    items[1].node->mark_dirty(ui::Dirty::Paint);
                    changed = true;
                } else if (input.type == ui::InputEventType::PointerMove) {
                    items[2].node->set_bounds({input.x - 36.0f, input.y - 36.0f, 72.0f, 72.0f});
                    items[2].node->mark_dirty(ui::Dirty::Layout | ui::Dirty::Paint);
                    changed = true;
                }
                if (changed) {
                    cause_pending = true;
                    if (!input_pending) {
                        // Latency runs from the first unrendered input to its frame being
                        // presented.
                        input_pending = true;
                        first_input_time = input.timestamp_s;
                    }
                }
            }

            while (next_script < kScripted.size() && now - start >= kScripted[next_script]) {
                items[1].color =
                    next_script % 2 == 0 ? rhi::rgba8(251, 146, 60) : rhi::rgba8(56, 189, 248);
                items[1].node->mark_dirty(ui::Dirty::Paint);
                ++next_script;
                ++scripted_fired;
                ++scripted_unrendered;
                cause_pending = true;
            }

            if (first_frame || root.subtree_dirty() != ui::Dirty::None ||
                root.dirty() != ui::Dirty::None) {
                demand_loop.mark_render_dirty();
            }
            const runtime::RuntimeTick tick = demand_loop.tick();
            if (!tick.should_render) {
                // Idle: park until the next scripted change or the end of the run. Window events
                // wake the wait early. No frame is recorded and nothing is presented.
                ++idle_waits;
                const double next_wake = next_script < kScripted.size()
                                             ? start + kScripted[next_script]
                                             : start + run_seconds;
                (void)hal::wait_for_events_until(*window, next_wake);
                continue;
            }

            const runtime::PaceDecision pace = pacer.ready(now);
            if (!pace.render_now) {
                (void)hal::wait_for_events_until(*window, now + pace.wait_seconds);
                continue;
            }

            (void)pipeline.tick(root, now - last_tick);
            last_tick = now;
            const ui::frame::FrameRender plan = pipeline.finish_frame(root);
            if (!first_frame && !plan.should_render) {
                dirty_pass.clear(root, ui::Dirty::All);
                demand_loop.clear_render_dirty();
                continue;
            }

            const FrameOutcome outcome = render_present([&](std::uint32_t, std::uint32_t) {
                for (const Item& item : items) {
                    renderer.push_rect(item.node->world_bounds(), item.color);
                }
            });
            if (!outcome.rendered) {
                continue;
            }
            ++rendered;
            presented += outcome.presented ? 1U : 0U;
            if (input_pending) {
                input_latency_ms.push_back((hal::steady_now_seconds() - first_input_time) * 1000.0);
                input_pending = false;
            }
            if (!cause_pending) {
                ++unjustified;
            }
            cause_pending = false;
            first_frame = false;
            scripted_unrendered = 0;
            dirty_pass.clear(root, ui::Dirty::All);
            demand_loop.clear_render_dirty();
        }

        window->set_input_queue(nullptr); // the queue is local; the window outlives it
        device->wait_idle();
        std::cout << "on-demand: rendered=" << rendered << " presented=" << presented
                  << " scripted=" << scripted_fired << " events=" << events_seen
                  << " idle_waits=" << idle_waits << " unjustified=" << unjustified
                  << " missed=" << scripted_unrendered << "\n";
        if (input_seen > 0) {
            double total = 0.0;
            double worst = 0.0;
            for (const double value : input_latency_ms) {
                total += value;
                worst = std::max(worst, value);
            }
            std::cout << "on-demand input: events=" << input_seen
                      << " frames=" << input_latency_ms.size() << " latency_avg_ms="
                      << (input_latency_ms.empty() ? 0.0 : total / input_latency_ms.size())
                      << " latency_max_ms=" << worst << "\n";
        }
        // Every frame needs a cause (startup, a window event or a scene change), every scripted
        // change must reach a frame, and the loop must have idled in between.
        const bool ok = unjustified == 0 && scripted_unrendered == 0 && idle_waits > 0 &&
                        rendered >= 1 + scripted_fired;
        return ok ? 0 : 1;
    }

    // The loop owns "should this iteration render" and the fixed-step update count. This demo has
    // no scene to go dirty, so it runs continuously; --on-demand is the scene-driven variant.
    loop.set_continuous(true);
    std::uint64_t fixed_steps = 0;

    bool closed = false;
    for (std::uint32_t frame = 0; frame < options.frames && !closed; ++frame) {
        const std::size_t event_count = window->poll_events(events);
        for (std::size_t i = 0; i < event_count; ++i) {
            if (events[i].type == hal::WindowEventType::Close) {
                closed = true;
            }
        }
        if (closed) {
            break;
        }

        (void)runtime::wait_for_next_frame(*window, pacer);
        window->poll_events(events);

        const runtime::RuntimeTick tick = loop.tick();
        fixed_steps += tick.steps;
        if (!tick.should_render) {
            continue;
        }

        const FrameOutcome outcome = render_present([&](std::uint32_t width, std::uint32_t height) {
            if (options.shapes) {
                build_shapes(renderer, *checker, width, height);
            } else {
                build_screen(renderer, *checker, width, height);
            }
        });
        rendered += outcome.rendered ? 1U : 0U;
        presented += outcome.presented ? 1U : 0U;
    }

    // Shutdown only: let queued frames finish before the swapchain and buffers are destroyed.
    device->wait_idle();
    std::cout << "windowed: rendered=" << rendered << " presented=" << presented
              << " steps=" << fixed_steps << "\n";
    return 0;
}

void build_cpu_screen(tessera::rendering::software::CpuCanvas& canvas) {
    using tessera::rendering::rhi::Rect;
    using tessera::rendering::rhi::rgba8;

    const auto background = rgba8(11, 18, 32);
    const auto panel = rgba8(15, 23, 42);
    const auto card = rgba8(30, 41, 59);
    const auto accent = rgba8(56, 189, 248);
    const auto green = rgba8(74, 222, 128);
    const auto yellow = rgba8(250, 204, 21);
    const auto muted = rgba8(148, 163, 184);
    const auto text = rgba8(226, 232, 240);
    const float width = static_cast<float>(canvas.width());
    const float height = static_cast<float>(canvas.height());

    canvas.clear(background);
    canvas.fill_rect({0, 0, width, 48}, panel);
    canvas.fill_rect({0, 0, width, 3}, accent);
    canvas.fill_rect({0, 48, 120, height - 48}, panel);
    canvas.fill_rect({12, 64, 96, 28}, accent);
    canvas.fill_rect({12, 100, 96, 28}, card);
    canvas.fill_rect({12, 136, 96, 28}, card);

    canvas.fill_rect({140, 64, 160, 104}, card);
    canvas.fill_rect({152, 76, 40, 40}, accent);
    canvas.fill_rect({204, 82, 80, 8}, text);
    canvas.fill_rect({204, 98, 56, 6}, muted);
    canvas.fill_rect({152, 128, 136, 6}, card);
    canvas.fill_rect({152, 128, 92, 6}, green);

    canvas.fill_rect({316, 64, 180, 104}, card);
    canvas.fill_rect({328, 76, 72, 8}, text);
    canvas.fill_rect({328, 92, 156, 56}, panel);
    for (std::uint32_t bar = 0; bar < 5; ++bar) {
        const float bar_height = 12.0f + static_cast<float>(bar) * 8.0f;
        canvas.fill_rect(
            {336.0f + static_cast<float>(bar) * 30.0f, 148.0f - bar_height, 20, bar_height},
            bar % 2 == 0 ? accent : green);
    }

    canvas.fill_rect({12, 184, 96, 8}, muted);
    canvas.fill_rect({12, 200, 72, 8}, card);
    canvas.fill_rect({140, 184, 356, 8}, text);
    canvas.fill_rect({140, 200, 280, 6}, muted);
    canvas.fill_rect({140, 220, 220, 6}, yellow);
}

namespace {

using tessera::rendering::rhi::Rect;
using tessera::rendering::rhi::rgba8;
using tessera::ui::NodeId;

class CpuStyles final : public tessera::ui::paint::StyleProvider {
public:
    void set(NodeId id, tessera::ui::style::ComputedStyle style) {
        styles_[id] = std::move(style);
    }

    tessera::ui::style::ComputedStyle compute(const tessera::ui::Node& node) override {
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? tessera::ui::style::ComputedStyle{} : found->second;
    }

private:
    std::map<NodeId, tessera::ui::style::ComputedStyle> styles_;
};

tessera::ui::style::ComputedStyle
cpu_style(std::initializer_list<std::pair<const char*, const char*>> values) {
    tessera::ui::style::ComputedStyle style;
    for (const auto& [property, value] : values) {
        style.set(property, value);
    }
    return style;
}

// Deterministic CPU text scene: paint pass -> PaintData runs (single-line + wrapped) ->
// CpuPaintSink glyph coverage compositing. No GPU, no timers, same test-font fixture as the GPU
// text golden.
void build_cpu_text_screen(tessera::rendering::software::CpuCanvas& canvas,
                           const Options& options) {
    if (options.font.empty()) {
        throw std::runtime_error("--cpu-text requires --font <fixture.ttf>");
    }
    auto loaded = tessera::ui::text::FontFace::load_from_file(options.font);
    if (!loaded) {
        throw std::runtime_error("failed to load font: " + loaded.error().message);
    }
    tessera::ui::text::FontFace face = std::move(*loaded);
    tessera::ui::text::GlyphRasterizer rasterizer(face);
    tessera::ui::text::HarfBuzzShaper shaper(face);
    tessera::poc::batched2d::CpuPaintSink sink(canvas, rasterizer, shaper);

    const auto background = rgba8(11, 18, 32);
    const auto green = rgba8(74, 222, 128);
    const auto muted = rgba8(148, 163, 184);
    const auto text_color = rgba8(226, 232, 240);
    const float width = static_cast<float>(canvas.width());
    const float height = static_cast<float>(canvas.height());

    tessera::ui::Node root(1);
    root.set_bounds({0, 0, width, height});

    auto& header = root.add_child(std::make_unique<tessera::ui::Node>(2));
    header.set_bounds({0, 0, width, 36});
    auto& accent_line = root.add_child(std::make_unique<tessera::ui::Node>(3));
    accent_line.set_bounds({0, 0, width, 3});
    auto& title = root.add_child(std::make_unique<tessera::ui::Node>(4));
    title.set_bounds({16, 10, width - 32.0f, 26});

    auto& left_card = root.add_child(std::make_unique<tessera::ui::Node>(5));
    left_card.set_bounds({16, 52, 140, 92});
    auto& left_text = root.add_child(std::make_unique<tessera::ui::Node>(6));
    left_text.set_bounds({24, 60, 124, 76});

    auto& right_card = root.add_child(std::make_unique<tessera::ui::Node>(7));
    right_card.set_bounds({width - 148.0f, 52, 132, 92});
    auto& right_text = root.add_child(std::make_unique<tessera::ui::Node>(8));
    right_text.set_bounds({width - 140.0f, 60, 116, 76});

    CpuStyles styles;
    styles.set(1, cpu_style({{"background-color", "#0B1220"}}));
    tessera::ui::paint::TextLayoutCache text_cache;
    tessera::ui::paint::PaintContext paint_context;
    paint_context.text_layout_cache = &text_cache;
    styles.set(2, cpu_style({{"background-color", "#0F172A"}}));
    styles.set(3, cpu_style({{"background-color", "#38BDF8"}}));
    styles.set(4, cpu_style({{"color", "#E2E8F0"}, {"font-size", "20px"}}));
    styles.set(5, cpu_style({{"background-color", "#1E293B"}}));
    styles.set(6, cpu_style({{"color", "#94A3B8"},
                             {"font-size", "13px"},
                             {"padding-left", "2px"},
                             {"padding-right", "2px"}}));
    styles.set(7, cpu_style({{"background-color", "#1E293B"}}));
    styles.set(8, cpu_style({{"color", "#4ADE80"}, {"font-size", "13px"}}));

    tessera::ui::PaintData title_data;
    title_data.text = options.text;
    title_data.text_style.font_size = options.text_size;
    title_data.text_style.color = text_color;
    title.set_paint_data(std::move(title_data));

    tessera::ui::PaintData left_data;
    left_data.text = "The quick brown fox jumps over the lazy dog near the river bank at dawn";
    left_data.wrap = tessera::ui::PaintWrap::Wrap;
    left_data.max_lines = 5;
    left_data.text_style.font_size = 13.0f;
    left_data.text_style.color = muted;
    left_text.set_paint_data(std::move(left_data));

    tessera::ui::PaintData right_data;
    right_data.text = "Deterministic coverage compositing for software goldens.";
    right_data.wrap = tessera::ui::PaintWrap::Wrap;
    right_data.max_lines = 5;
    right_data.text_align = tessera::ui::PaintTextAlign::Center;
    right_data.text_style.font_size = 13.0f;
    right_data.text_style.color = green;
    right_text.set_paint_data(std::move(right_data));

    canvas.clear(background);
    const tessera::ui::paint::PaintStats stats =
        tessera::ui::paint::paint_scene(root, styles, sink, paint_context);
    std::cout << "cpu-text: nodes=" << stats.nodes << " texts=" << stats.texts
              << " fills=" << stats.backgrounds + stats.borders << "\n";
}

} // namespace

int run_cpu(const Options& options) {
    using namespace tessera::rendering;

    software::CpuCanvas canvas(options.width, options.height);
    const bool text_scene = options.cpu_text;
    if (text_scene) {
        const auto started = std::chrono::steady_clock::now();
        build_cpu_text_screen(canvas, options);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        std::cout << "cpu-text: render_ms="
                  << std::chrono::duration<double, std::milli>(elapsed).count() << "\n";
    } else {
        build_cpu_screen(canvas);
    }
    const std::vector<std::uint8_t> pixels(canvas.pixels().begin(), canvas.pixels().end());

    if (!options.raw.empty()) {
        write_raw(options.raw, pixels);
    }
    if (!options.out.empty()) {
        const int wrote = stbi_write_png(options.out.c_str(), static_cast<int>(canvas.width()),
                                         static_cast<int>(canvas.height()), 4, pixels.data(),
                                         static_cast<int>(canvas.width()) * 4);
        if (wrote == 0) {
            throw std::runtime_error("failed to write PNG: " + options.out);
        }
    }
    compare_with_golden(options, pixels);

    std::cout << "cpu: pixels=" << pixels.size() / 4
              << " wrote=" << (options.out.empty() ? "<none>" : options.out) << "\n";
    return 0;
}

// Hardened windowed path: offscreen 4x target resolved into the present image (readback bridge,
// because RHI has no image-to-image copy yet; swapchain images are always 1-sample), plus the D7
// DeviceRecoveryController with a test-hook loss injection. Kept separate from the default windowed
// loop so `--msaa`/`--simulate-loss` do not change the existing demo/tests.
int run_windowed_hardened(const Options& options) {
    using namespace tessera;
    using namespace tessera::rendering;

    if (options.font.empty()) {
        std::cout
            << "windowed-hardened: skipped (--msaa/--simulate-loss require --font <fixture>)\n";
        return 0;
    }
#if !defined(TESSERA_VULKAN_TEST_HOOKS)
    if (options.simulate_loss) {
        std::cout
            << "windowed-hardened: skipped (--simulate-loss requires TESSERA_VULKAN_TEST_HOOKS)\n";
        return 0;
    }
#endif
    if (options.msaa != 1 && options.msaa != 4) {
        std::cout << "windowed-hardened: unsupported --msaa " << options.msaa
                  << " (expected 1 or 4)\n";
        return 2;
    }

    hal::WindowDesc window_desc;
    window_desc.title = "Tessera windowed POC (hardened)";
    window_desc.width = options.width;
    window_desc.height = options.height;
    window_desc.visible = false;
    auto window = hal::create_window(window_desc);
    if (window == nullptr) {
        std::cout << "windowed-hardened: skipped (no platform window backend)\n";
        return 0;
    }
    const hal::NativeWindowHandle native = window->native_handle();
    if (native.window == nullptr) {
        std::cout << "windowed-hardened: skipped (window has no native handle)\n";
        return 0;
    }

    auto device =
        vulkan::create_vulkan_device(rhi::DeviceDesc{rhi::Backend::Vulkan, options.validation, -1});

    auto loaded = ui::text::FontFace::load_from_file(options.font);
    if (!loaded) {
        throw std::runtime_error("windowed-hardened: failed to load font: " +
                                 loaded.error().message);
    }

    std::unique_ptr<poc::batched2d::TextDemo> text_demo;
    std::unique_ptr<rhi::Swapchain> swapchain;
    std::unique_ptr<rhi::Pipeline> pipeline;
    std::unique_ptr<gpu::Batched2DRenderer> renderer;
    std::unique_ptr<rhi::Pipeline> msaa_pipeline;
    std::unique_ptr<gpu::Batched2DRenderer> msaa_renderer;
    std::unique_ptr<rhi::RenderTarget> offscreen;
    std::unique_ptr<rhi::Texture> present_texture;
    std::unique_ptr<rhi::Texture> white;

    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    const auto create_resources = [&] {
        const hal::WindowMetrics metrics = window->metrics();
        rhi::SwapchainDesc swap_desc;
        swap_desc.extent = {metrics.physical_width, metrics.physical_height};
        swap_desc.image_count = 2;
        swap_desc.vsync = true;
        swap_desc.native_window = native.window;
        swap_desc.native_display = native.display;
        auto created = device->create_swapchain(swap_desc);
        if (!created) {
            throw std::runtime_error("windowed-hardened: create_swapchain failed");
        }
        swapchain = std::move(*created);

        rhi::TextureDesc white_desc;
        white_desc.extent = {1, 1};
        white_desc.pixels = white_pixel.data();
        white_desc.row_pitch = 4;
        white = device->create_texture(white_desc);

        auto pipeline_desc = vulkan::batched2d_pipeline_desc();
        pipeline_desc.color_format = swapchain->format();
        pipeline = device->create_pipeline(pipeline_desc);
        gpu::Batched2DConfig config{4096, true, true};
        renderer = std::make_unique<gpu::Batched2DRenderer>(*device, *pipeline, *white, config);
        text_demo = std::make_unique<poc::batched2d::TextDemo>(*device, *loaded, options.text,
                                                               options.text_size);

        if (options.msaa > 1) {
            rhi::RenderTargetDesc msaa_desc;
            msaa_desc.extent = swapchain->extent();
            msaa_desc.format = swapchain->format();
            msaa_desc.clear_color = rhi::rgba8(11, 18, 32);
            msaa_desc.sample_count = options.msaa;
            offscreen = device->create_render_target(msaa_desc);
            auto msaa_pipeline_desc =
                vulkan::batched2d_pipeline_desc(swapchain->format(), options.msaa);
            msaa_pipeline = device->create_pipeline(msaa_pipeline_desc);
            gpu::Batched2DConfig msaa_config{4096, true, true, options.msaa};
            msaa_renderer = std::make_unique<gpu::Batched2DRenderer>(*device, *msaa_pipeline,
                                                                     *white, msaa_config);
            rhi::TextureDesc present_desc;
            present_desc.extent = swapchain->extent();
            present_desc.format = swapchain->format();
            present_desc.filter = rhi::Filter::Linear;
            present_texture = device->create_texture(present_desc);
        }
    };

    const auto destroy_resources = [&] {
        text_demo.reset();
        present_texture.reset();
        msaa_renderer.reset();
        msaa_pipeline.reset();
        offscreen.reset();
        renderer.reset();
        pipeline.reset();
        white.reset();
        swapchain.reset();
    };

    const auto paint_scene = [&](gpu::Batched2DRenderer& frame_renderer, std::uint32_t width,
                                 std::uint32_t height) {
        const float card_width = static_cast<float>(width) * 0.52f;
        const float card_height = static_cast<float>(height) * 0.62f;
        const rhi::Rect card{static_cast<float>(width) * 0.06f, static_cast<float>(height) * 0.18f,
                             card_width, card_height};
        const gpu::clip::RoundedRect clip{card, 18.0f, 18.0f, 18.0f, 18.0f};
        frame_renderer.push_clip(clip);
        frame_renderer.push_rounded_rect(card, rhi::rgba8(24, 32, 48), 18.0f);
        text_demo->begin_frame();
        (void)text_demo->draw_wrapped(
            frame_renderer,
            "The quick brown fox jumps over the lazy dog near the river bank at dawn",
            card.x + 12.0f, card.y + 28.0f, card.width - 24.0f, 15, rhi::rgba8(226, 232, 240));
        frame_renderer.push_transform(rhi::Mat3::translation(static_cast<float>(width) * 0.76f,
                                                             static_cast<float>(height) * 0.58f) *
                                      rhi::Mat3::rotation(0.42f));
        frame_renderer.push_rect({-34.0f, -34.0f, 68.0f, 68.0f}, rhi::rgba8(56, 189, 248));
        frame_renderer.pop_transform();
        frame_renderer.pop_clip();
    };

    struct FrameOutcome {
        bool rendered = false;
        bool presented = false;
    };

    const auto render_present = [&]() -> FrameOutcome {
        FrameOutcome outcome;
        if (!swapchain) {
            return outcome;
        }
        const hal::WindowMetrics metrics = window->metrics();
        if (metrics.physical_width != swapchain->extent().width ||
            metrics.physical_height != swapchain->extent().height) {
            swapchain->resize({metrics.physical_width, metrics.physical_height});
            if (options.msaa > 1) {
                present_texture.reset();
                offscreen.reset();
                rhi::RenderTargetDesc msaa_desc;
                msaa_desc.extent = swapchain->extent();
                msaa_desc.format = swapchain->format();
                msaa_desc.clear_color = rhi::rgba8(11, 18, 32);
                msaa_desc.sample_count = options.msaa;
                offscreen = device->create_render_target(msaa_desc);
                rhi::TextureDesc present_desc;
                present_desc.extent = swapchain->extent();
                present_desc.format = swapchain->format();
                present_desc.filter = rhi::Filter::Linear;
                present_texture = device->create_texture(present_desc);
            }
        }

        std::optional<std::uint32_t> image;
        for (int attempt = 0; attempt < 4 && !image; ++attempt) {
            const auto acquired = swapchain->acquire_next_image(100);
            if (acquired) {
                image = *acquired;
                break;
            }
            if (acquired.error() == rhi::SwapchainError::OutOfDate) {
                swapchain->resize({metrics.physical_width, metrics.physical_height});
                continue;
            }
            break;
        }
        if (!image) {
            return outcome;
        }
        rhi::RenderTarget* target = swapchain->image_target(*image);
        if (target == nullptr) {
            return outcome;
        }

        rhi::RenderTargetDesc target_desc;
        target_desc.extent = swapchain->extent();
        target_desc.format = swapchain->format();
        target_desc.load_op = rhi::LoadOp::Clear;
        target_desc.clear_color = rhi::rgba8(11, 18, 32);

        if (options.msaa > 1) {
            // Offscreen 4x: resolve happens at end of its render pass; the CPU readback bridge then
            // uploads the resolved 1-sample image and draws it into the 1-sample present image.
            if (!offscreen || !present_texture) {
                return outcome;
            }
            auto msaa_commands = device->create_command_list();
            rhi::RenderTargetDesc msaa_desc;
            msaa_desc.extent = swapchain->extent();
            msaa_desc.format = swapchain->format();
            msaa_desc.clear_color = rhi::rgba8(11, 18, 32);
            msaa_desc.sample_count = options.msaa;
            msaa_renderer->begin(*msaa_commands, *offscreen, msaa_desc);
            paint_scene(*msaa_renderer, target_desc.extent.width, target_desc.extent.height);
            msaa_renderer->end();
            msaa_commands->submit();
            device->wait_idle();

            const std::size_t bytes = static_cast<std::size_t>(target_desc.extent.width) *
                                      target_desc.extent.height * sizeof(std::uint32_t);
            auto readback = device->create_buffer({bytes, rhi::BufferUsage::Readback});
            auto copy_commands = device->create_command_list();
            copy_commands->copy_target_to_buffer(*offscreen, *readback);
            copy_commands->submit();
            std::vector<std::uint8_t> pixels(bytes);
            readback->read(pixels.data(), pixels.size());
            present_texture->write(pixels.data(), pixels.size());

            auto commands = device->create_command_list();
            renderer->begin(*commands, *target, target_desc);
            renderer->push_texture({0.0f, 0.0f, static_cast<float>(target_desc.extent.width),
                                    static_cast<float>(target_desc.extent.height)},
                                   *present_texture, {0.0f, 0.0f, 1.0f, 1.0f},
                                   rhi::rgba8(255, 255, 255));
            renderer->end();
            commands->submit();
        } else {
            auto commands = device->create_command_list();
            renderer->begin(*commands, *target, target_desc);
            paint_scene(*renderer, target_desc.extent.width, target_desc.extent.height);
            renderer->end();
            commands->submit();
        }
        outcome.rendered = true;

        const auto result = swapchain->present(*image);
        outcome.presented = result.has_value() || result.error() == rhi::SwapchainError::Suboptimal;
        return outcome;
    };

    runtime::SteadyClock clock;
    runtime::RuntimeLoopConfig loop_config;
    runtime::DeviceRecoveryController controller(*device);
    loop_config.device_recovery = &controller;
    runtime::RuntimeLoop loop(clock, loop_config);
    loop.set_continuous(true);

    std::array<hal::WindowEvent, 16> events{};
    std::uint32_t rendered_before = 0;
    std::uint32_t rendered_after = 0;
    std::uint32_t presented = 0;
    std::uint32_t blocked_ticks = 0;
    std::uint32_t recovery_tick = 0;
    bool loss_injected = false;
    bool recovered = false;
    bool closed = false;
    create_resources();

#if defined(TESSERA_VULKAN_TEST_HOOKS)
    auto* vulkan_device = dynamic_cast<vulkan::VulkanDevice*>(device.get());
#endif

    constexpr std::uint32_t kPreLossFrames = 3;
    constexpr std::uint32_t kMaxTicks = 5000;
    for (std::uint32_t tick_index = 0; tick_index < kMaxTicks && !closed; ++tick_index) {
        const std::size_t event_count = window->poll_events(events);
        for (std::size_t i = 0; i < event_count; ++i) {
            if (events[i].type == hal::WindowEventType::Close) {
                closed = true;
            }
        }
        if (closed) {
            break;
        }

#if defined(TESSERA_VULKAN_TEST_HOOKS)
        if (options.simulate_loss && !loss_injected && rendered_before >= kPreLossFrames &&
            vulkan_device != nullptr) {
            destroy_resources();
            vulkan_device->simulate_device_loss_for_testing();
            loss_injected = true;
            std::cout << "windowed-hardened: injected device loss after " << rendered_before
                      << " frames\n";
        }
#endif

        const runtime::RuntimeTick tick = loop.tick();
        if (tick.device_blocked) {
            // The controller is WaitingForCaller: release the lost-generation objects so the next
            // tick can attempt recovery.
            destroy_resources();
            ++blocked_ticks;
            continue;
        }
        if (tick.device_recovered) {
            create_resources();
            recovered = true;
            recovery_tick = tick_index + 1;
            std::cout << "windowed-hardened: recovered at tick " << recovery_tick
                      << " generation=" << tick.device_generation << "\n";
            continue;
        }
        if (!tick.should_render) {
            continue;
        }
        if (!swapchain) {
            create_resources();
        }
        const FrameOutcome outcome = render_present();
        if (outcome.rendered) {
            if (loss_injected) {
                ++rendered_after;
            } else {
                ++rendered_before;
            }
        }
        presented += outcome.presented ? 1U : 0U;
        if (loss_injected && !recovered) {
            continue;
        }
        if (!options.simulate_loss) {
            if (rendered_before >= options.frames) {
                break;
            }
        } else if (recovered && rendered_after >= options.frames && rendered_before >= 1) {
            break;
        }
    }

    destroy_resources();
    device->wait_idle();
    const bool ok = rendered_before >= 1 && presented >= 1 &&
                    (!options.simulate_loss ||
                     (loss_injected && recovered && rendered_after >= options.frames));
    std::cout << "windowed-hardened: msaa=" << options.msaa << " frames_before=" << rendered_before
              << " frames_after=" << rendered_after << " presented=" << presented
              << " blocked_ticks=" << blocked_ticks << " recovery_tick=" << recovery_tick
              << " injected=" << loss_injected << " recovered=" << recovered << "\n";
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        return 2;
    }

    try {
        if (options.backend == "null") {
            return run_null(options);
        }
        if (options.backend == "cpu") {
            return run_cpu(options);
        }
        if (options.backend == "vulkan") {
            if (options.windowed) {
                if (options.msaa > 1 || options.simulate_loss) {
                    return run_windowed_hardened(options);
                }
                return run_windowed(options);
            }
            return run_vulkan(options);
        }
        std::cerr << "unsupported backend: " << options.backend << "\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
