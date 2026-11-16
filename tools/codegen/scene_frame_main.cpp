// SPDX-License-Identifier: MIT

// tessera_scene_frame: the native half of the preview's canvas parity gate (issue #167, P3 of
// docs/design/reference-equantic-ui.md section 7.4).
//
//   tessera_scene_frame --png OUT FILE.tsx [--backend vulkan|null] [--size WxH@DPI] [options]
//
// It runs the same pipeline the browser module does - `scene_frame.cpp`, which shares
// `scene_prepare.cpp` with the P2 snapshot driver - and writes the rendered frame as PNG. The point
// is a *comparable artifact*: the browser's WebGPU frame and this one differ only in the RHI
// device, so a tolerance can be applied to two images of the same program instead of to two
// programs.
//
// Two backends, and the difference matters for what the gate proves:
//
//   --backend vulkan   a real offscreen render. On a lavapipe host (the Linux CI lane, ADR-009) it
//                      writes the reference image the canvas is compared against; on a machine with
//                      a hardware driver it is evidence that the path works, never a golden.
//   --backend null     records the command stream and writes no pixels (`--dump-commands` prints
//                      what it recorded). This is the half a host without a GPU can still prove.
//
// Exit codes: 0 frame written (or the command stream dumped); 1 the source produced no scene;
// 2 usage/setup, including a backend this build does not have.

#include "preview_text.hpp"
#include "scene_frame.hpp"
#include "scene_prepare.hpp"
#include "tessera/harness/image_compare.hpp"
#include "tessera/rendering/null/null_device.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(TESSERA_SCENE_FRAME_HAS_VULKAN)
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#endif

namespace {

namespace rhi = tessera::rendering::rhi;

constexpr std::string_view kUsage =
    "usage: tessera_scene_frame --png OUT FILE.tsx [options]\n"
    "\n"
    "  --png OUT          write the rendered frame as PNG (`-` writes the command dump instead)\n"
    "  --backend NAME     vulkan (default where available) or null\n"
    "  --size WxH[@DPI]   the physical target extent; `@DPI` sizes a logical viewport up front\n"
    "                     (default: 320x240@1)\n"
    "  --component NAME   the component to preview (default: the first in the file)\n"
    "  --file NAME        the project-relative source name the ids are hashed with\n"
    "  --salt SALT        the project salt the ids are hashed with (default: preview)\n"
    "  --font PATH        the font bytes to paint text with. Omit to paint boxes only, which\n"
    "                     counts every run in text_skipped (the pre-#387 behaviour)\n"
    "  --dump-commands    print the frame's draw counters (works with the null backend)\n"
    "\n"
    "Exit codes: 0 frame written; 1 no scene (the source produced no tree); 2 usage/setup.\n";

int usage_error(std::string_view message) {
    std::cerr << "tessera_scene_frame: " << message << "\n\n" << kUsage;
    return 2;
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// The font bytes as the module would receive them: one flat buffer, no path remembered. Reading the
// file here rather than in the stack keeps `preview_text.cpp` free of `<fstream>`, so the same
// object builds for a browser that has no filesystem at all.
bool read_font(const std::string& path, std::vector<std::uint8_t>& out) {
    std::string bytes;
    if (!read_file(path, bytes)) {
        return false;
    }
    out.assign(bytes.begin(), bytes.end());
    return true;
}

// `WxH` or `WxH@DPI`, all positive and finite. The logical viewport is what the layout engine sizes
// the scene to; the *physical* extent is logical x DPI, which is the target the frame renders into.
// Keeping the two explicit is the whole point: a page that laid out at 320x240 and rendered at its
// device-pixel size must not be confused with one that laid out at the device size.
bool parse_size(std::string_view text, float& width, float& height, float& dpi) {
    const std::size_t at = text.find('@');
    const std::string_view box = text.substr(0, at);
    if (at != std::string_view::npos) {
        try {
            dpi = std::stof(std::string(text.substr(at + 1)));
        } catch (const std::exception&) {
            return false;
        }
    }
    const std::size_t x = box.find('x');
    if (x == std::string_view::npos) {
        return false;
    }
    try {
        width = std::stof(std::string(box.substr(0, x)));
        height = std::stof(std::string(box.substr(x + 1)));
    } catch (const std::exception&) {
        return false;
    }
    const auto bad = [](float value) {
        return !std::isfinite(value) || value <= 0.0f || value > 65535.0f;
    };
    return !bad(width) && !bad(height) && std::isfinite(dpi) && dpi > 0.0f && dpi <= 8.0f;
}

std::unique_ptr<rhi::Device> create_device(std::string_view backend, std::string& error) {
    if (backend == "null") {
        return tessera::rendering::null::create_null_device(rhi::DeviceDesc{});
    }
#if defined(TESSERA_SCENE_FRAME_HAS_VULKAN)
    if (backend == "vulkan" || backend.empty()) {
        rhi::DeviceDesc desc;
        desc.backend = rhi::Backend::Vulkan;
        desc.validation = false;
        desc.device_index = -1;
        try {
            return tessera::rendering::vulkan::create_vulkan_device(desc);
        } catch (const std::exception& failure) {
            error = std::string("no Vulkan device: ") + failure.what();
            return nullptr;
        }
    }
    error = "this build has no Vulkan backend; pass --backend null";
    return nullptr;
#else
    error = "this build has no Vulkan backend; pass --backend null";
    return nullptr;
#endif
}

} // namespace

int main(int argc, char** argv) {
    std::string output;
    std::string component;
    std::string project_file;
    std::string salt = "preview";
    std::string backend = "vulkan";
    std::string font_path;
    float width = 320.0f;
    float height = 240.0f;
    float dpi = 1.0f;
    bool dump_commands = false;
    std::string input;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            std::cout << kUsage;
            return 0;
        }
        if (arg == "--dump-commands") {
            dump_commands = true;
            continue;
        }
        if (arg == "--png" || arg == "--component" || arg == "--salt" || arg == "--size" ||
            arg == "--file" || arg == "--backend" || arg == "--font") {
            if (i + 1 >= argc) {
                return usage_error("`" + std::string(arg) + "` needs a value");
            }
            const std::string value(argv[++i]);
            if (arg == "--png") {
                output = value;
            } else if (arg == "--component") {
                component = value;
            } else if (arg == "--file") {
                project_file = value;
            } else if (arg == "--salt") {
                salt = value;
            } else if (arg == "--backend") {
                backend = value;
            } else if (arg == "--font") {
                font_path = value;
            } else if (!parse_size(value, width, height, dpi)) {
                return usage_error("`--size` takes WxH or WxH@DPI, for example 320x240@2");
            }
            continue;
        }
        if (!arg.empty() && arg.front() == '-') {
            return usage_error("unknown option `" + std::string(arg) + "`");
        }
        if (!input.empty()) {
            return usage_error("one input .tsx file");
        }
        input = std::string(arg);
    }

    if (output.empty()) {
        return usage_error("--png is required");
    }
    if (input.empty()) {
        return usage_error("no input .tsx file");
    }
    std::string source;
    if (!read_file(input, source)) {
        std::cerr << "tessera_scene_frame: cannot read `" << input << "`\n";
        return 2;
    }

    std::string device_error;
    std::unique_ptr<rhi::Device> device = create_device(backend, device_error);
    if (device == nullptr) {
        std::cerr << "tessera_scene_frame: " << device_error << "\n";
        return 2;
    }

    // The font is loaded from bytes, exactly as the page hands them to the browser module: one flat
    // buffer, no path retained, nothing fetched. `--font` is optional so the font-free frame (boxes
    // only, `text_skipped` counted) stays reproducible for the older gate.
    std::unique_ptr<tessera::compiler::codegen::cli::PreviewTextStack> text;
    if (!font_path.empty()) {
        std::vector<std::uint8_t> font_bytes;
        if (!read_font(font_path, font_bytes)) {
            std::cerr << "tessera_scene_frame: cannot read `" << font_path << "`\n";
            return 2;
        }
        auto loaded =
            tessera::compiler::codegen::cli::PreviewTextStack::create(font_bytes, *device);
        if (!loaded) {
            std::cerr << "tessera_scene_frame: the font is unusable: " << loaded.error().message
                      << "\n";
            return 2;
        }
        text = std::move(*loaded);
        std::cout << "tessera_scene_frame: font=" << text->face_name()
                  << " bytes=" << font_bytes.size() << "\n";
    }

    tessera::compiler::codegen::cli::ScenePreviewOptions options;
    options.file = project_file.empty() ? std::string_view(input) : std::string_view(project_file);
    options.salt = salt;
    // The layout viewport is the logical size; the target is the physical one.
    options.width = width;
    options.height = height;
    options.dpi_scale = dpi;
    const auto scaled = [dpi](float logical) {
        return static_cast<std::uint32_t>(std::ceil(logical * dpi));
    };
    const rhi::Extent2D extent{scaled(width), scaled(height)};

    // A Vulkan pipeline is built from SPIR-V words, so the shared shader-id description (which is
    // what WebGPU can create) is not usable here; the backend's own description carries the words
    // and the same vertex layout, blend mode and push-constant size.
    rhi::PipelineDesc pipeline = tessera::compiler::codegen::cli::scene_frame_pipeline_desc(
        rhi::TextureFormat::R8G8B8A8Unorm, 1);
#if defined(TESSERA_SCENE_FRAME_HAS_VULKAN)
    if (device->backend() == rhi::Backend::Vulkan) {
        pipeline = tessera::rendering::vulkan::batched2d_pipeline_desc(
            rhi::TextureFormat::R8G8B8A8Unorm, 1);
    }
#endif

    const auto result = tessera::compiler::codegen::cli::scene_frame_render(
        source, options, *device, extent, component, pipeline, text.get());
    if (!result.error.empty()) {
        std::cerr << "tessera_scene_frame: " << result.error << "\n";
        // A scene that could not be built is exit 1 (the same distinction the P2 CLI makes between
        // "the source produced no scene" and "the setup is wrong").
        return result.nodes == 0 ? 1 : 2;
    }

    std::cout << "tessera_scene_frame: backend=" << backend << " extent=" << extent.width << "x"
              << extent.height << " nodes=" << result.nodes << " draws=" << result.draw_calls
              << " vertices=" << result.vertex_count << " text_skipped=" << result.text_skipped
              << " glyphs_dropped=" << result.glyphs_dropped << "\n";

    if (dump_commands) {
        std::cout << "tessera_scene_frame: commands draws=" << result.draw_calls
                  << " vertices=" << result.vertex_count << " backgrounds=" << result.backgrounds
                  << " borders=" << result.borders << "\n";
    }

    if (!result.pixels_available) {
        // A backend with no synchronous readback (WebGPU in a browser) proves the command stream
        // and nothing about pixels; saying so is the difference between an honest lane and a green
        // one.
        std::cerr
            << "tessera_scene_frame: this backend cannot read pixels back synchronously, so no "
               "PNG was written\n";
        return 2;
    }

    tessera::harness::RgbaImage image;
    image.width = result.frame.width;
    image.height = result.frame.height;
    image.rgba = result.frame.rgba;
    const auto encoded = tessera::harness::encode_png(image);
    if (!encoded.has_value()) {
        std::cerr << "tessera_scene_frame: cannot encode the PNG: " << encoded.error().message
                  << "\n";
        return 2;
    }
    if (output == "-") {
        std::cout.write(encoded->data(), static_cast<std::streamsize>(encoded->size()));
        return 0;
    }
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "tessera_scene_frame: cannot write `" << output << "`\n";
        return 2;
    }
    out.write(encoded->data(), static_cast<std::streamsize>(encoded->size()));
    return out ? 0 : 2;
}
