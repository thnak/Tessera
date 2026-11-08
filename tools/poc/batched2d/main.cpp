// SPDX-License-Identifier: MIT

#include "damage_cli.hpp"
#include "tessera/rendering/golden/image_diff.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/null/null_device.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "text_demo.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
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
    std::string font;
    std::string text = "Tessera";
    float text_size = 24.0f;
    std::string compare;
    std::uint32_t tolerance = 2;
    double max_mismatch = 0.005;
    std::vector<tessera::rendering::rhi::Rect> damage;
};

void print_usage() {
    std::cout << "usage: batched2d_poc [--backend vulkan|null] [--out path] [--raw path]\n"
                 "                     [--width N] [--height N] [--validation] [--golden]\n"
                 "                     [--damage x,y,w,h]... [--font path.ttf] [--text string]\n"
                 "                     [--text-size N] [--compare golden.png] [--tolerance N]\n"
                 "                     [--max-mismatch ratio]\n";
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
                       tessera::rendering::gpu::Batched2DRenderer& renderer) {
    if (text == nullptr) {
        return;
    }
    text->begin_frame();
    text->draw(renderer, 150.0f, 34.0f, tessera::rendering::rhi::rgba8(226, 232, 240));
    std::cout << "text glyphs=" << text->atlas().stats().misses + text->atlas().stats().hits
              << " pages=" << text->atlas().page_count() << "\n";
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
    const std::span<const std::uint8_t> expected(decoded, static_cast<std::size_t>(width) *
                                                              static_cast<std::size_t>(height) * 4);
    const auto diff = tessera::rendering::golden::compare_rgba(
        {options.width, options.height}, pixels,
        {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)}, expected,
        options.tolerance);
    stbi_image_free(decoded);

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
    build_screen(renderer, *checker, options.width, options.height);
    draw_text_overlay(text_demo.get(), renderer);
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
    build_screen(renderer, *checker, options.width, options.height);
    draw_text_overlay(text_demo.get(), renderer);
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
        if (options.backend == "vulkan") {
            return run_vulkan(options);
        }
        std::cerr << "unsupported backend: " << options.backend << "\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
