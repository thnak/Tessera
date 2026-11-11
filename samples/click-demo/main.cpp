// SPDX-License-Identifier: MIT
//
// click-demo: the interactive Tessera UI acceptance for the app-host lane.
//
// Scene: the generated `demo::gen::Demo` component (samples/compiler-demo/Demo.tsx -> IR -> C++),
// styled by a real compiled stylesheet, laid out by Yoga, painted through the Batched2D backend.
//
// Modes:
//   click_demo --capture shot.png           offscreen capture of the first frame (no window; works
//                                            on any Vulkan device, including lavapipe in CI)
//   click_demo --click --capture after.png  dispatches a real pointer click on the generated Button
//                                            through the InputRouter before capturing
//   click_demo --windowed [--frames N]      visible Win32/X11 window, real mouse input, present
//   click_demo --windowed --hidden --frames 5  CI smoke: hidden window, five presented frames
//
// The window path uses two frames in flight and a bounded wait on the slot it reuses, like the
// batched2d POC. Swapchain images are not preserved, so a presented frame is always a full redraw;
// the host's damage plan still decides whether a frame is needed at all.

#include "scene.hpp"
#include "stb_image_write.h"
#include "tessera/app/host.hpp"
#include "tessera/hal/timer/steady_time.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/runtime/steady_clock.hpp"
#include "tessera/ui/input_pump.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/text_renderer.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef TESSERA_SAMPLE_FONT
#define TESSERA_SAMPLE_FONT "tests/fixtures/fonts/NotoSans-Regular.ttf"
#endif

namespace {

namespace rhi = tessera::rendering::rhi;
namespace gpu = tessera::rendering::gpu;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::Node;

struct Args {
    std::string capture;
    std::string font = TESSERA_SAMPLE_FONT;
    bool click = false;
    bool windowed = false;
    bool hidden = false;
    int frames = 0;
    int width = 380;
    int height = 260;
};

// One loaded font + the text stack Batched2DSink needs. Lives as long as the sink.
struct TextStack {
    TextStack(rhi::Device& device, const std::string& font_path)
        : face(load(device, font_path)),
          rasterizer(face),
          shaper(face),
          renderer(device, rasterizer, {1024, 1024, 2, 1}) {}

    static tessera::ui::text::FontFace load(rhi::Device& /*device*/, const std::string& path) {
        auto loaded = tessera::ui::text::FontFace::load_from_file(path);
        if (!loaded) {
            std::cerr << "click-demo: failed to load font " << path << ": "
                      << loaded.error().message << "\n";
            std::exit(2);
        }
        return *loaded;
    }

    tessera::ui::text::FontFace face;
    tessera::ui::text::GlyphRasterizer rasterizer;
    tessera::ui::text::HarfBuzzShaper shaper;
    tessera::ui::text::TextRenderer renderer;
};

std::unique_ptr<rhi::Device> create_device() {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::Vulkan;
    desc.validation = false;
    desc.device_index = -1;
    try {
        return tessera::rendering::vulkan::create_vulkan_device(desc);
    } catch (const std::exception& error) {
        std::cout << "click-demo: skipped (Vulkan device unavailable: " << error.what() << ")\n";
        return nullptr;
    }
}

std::unique_ptr<rhi::Texture> create_white(rhi::Device& device) {
    const std::array<std::uint8_t, 4> white = {255, 255, 255, 255};
    rhi::TextureDesc desc;
    desc.extent = {1, 1};
    desc.pixels = white.data();
    desc.row_pitch = 4;
    return device.create_texture(desc);
}

rhi::RenderTargetDesc target_desc(rhi::Extent2D extent, rhi::TextureFormat format) {
    rhi::RenderTargetDesc desc;
    desc.extent = extent;
    desc.format = format;
    desc.load_op = rhi::LoadOp::Clear;
    desc.store_op = rhi::StoreOp::Store;
    desc.clear_color = rhi::rgba8(11, 18, 32);
    return desc;
}

void add_root_style(Node& root) {
    // Host-owned geometry first (the AppHost ctor sets width/height/display/flex-direction); the
    // sample centers the card and paints the window background, which an app's stylesheet would do
    // through a `#root` selector once element ids are generated.
    root.set_inline_style("align-items", "center");
    root.set_inline_style("padding-top", "24px");
    root.set_inline_style("background-color", "#0b1220");
}

Node* find_kind(Node& node, std::string_view kind) {
    if (node.element_kind() == kind) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (Node* found = find_kind(*child, kind)) {
            return found;
        }
    }
    return nullptr;
}

InputEvent pointer_event(InputEventType type, float x, float y) {
    InputEvent event;
    event.type = type;
    event.x = x;
    event.y = y;
    event.pointer_id = 1;
    event.pointer_kind = tessera::ui::PointerKind::Mouse;
    event.button = tessera::ui::PointerButton::Left;
    event.is_primary = true;
    return event;
}

bool write_png(const std::string& path, rhi::Extent2D extent,
               const std::vector<std::uint8_t>& pixels) {
    return stbi_write_png(path.c_str(), static_cast<int>(extent.width),
                          static_cast<int>(extent.height), 4, pixels.data(),
                          static_cast<int>(extent.width) * 4) != 0;
}

// Dispatches a real pointer press/release through the scene input router at the first generated
// Button, which is the same path the window's InputPump uses.
bool click_generated_button(Node& root) {
    Node* button = find_kind(root, "Button");
    if (button == nullptr) {
        return false;
    }
    const rhi::Rect box = button->world_bounds();
    if (box.width <= 0.0f || box.height <= 0.0f) {
        return false;
    }
    tessera::ui::InputRouter router;
    const float x = box.x + box.width * 0.5f;
    const float y = box.y + box.height * 0.5f;
    (void)router.dispatch(root, pointer_event(InputEventType::PointerDown, x, y));
    (void)router.dispatch(root, pointer_event(InputEventType::PointerUp, x, y));
    return true;
}

int run_capture(const Args& args) {
    auto device = create_device();
    if (device == nullptr) {
        return 0;
    }
    const rhi::TextureFormat format = rhi::TextureFormat::R8G8B8A8Unorm;
    auto pipeline_desc = tessera::rendering::vulkan::batched2d_pipeline_desc();
    pipeline_desc.color_format = format;
    auto pipeline = device->create_pipeline(pipeline_desc);
    auto white = create_white(*device);
    gpu::Batched2DRenderer renderer(*device, *pipeline, *white,
                                    gpu::Batched2DConfig{4096, true, false});
    TextStack text(*device, args.font);
    tessera::ui::paint::Batched2DSink sink(renderer, &text.renderer, &text.shaper);

    tessera::runtime::ManualClock clock;
    tessera::runtime::RuntimeLoop loop(clock);
    tessera::app::HostConfig config;
    config.width = static_cast<float>(args.width);
    config.height = static_cast<float>(args.height);
    click_demo::DemoScene scene;
    add_root_style(scene.root);
    tessera::app::AppHost host(scene.root, click_demo::demo_stylesheet(), loop, config);
    host.set_update_scheduler(&scene.scheduler);
    (void)host.begin_frame(0.0);
    if (args.click && !click_generated_button(scene.root)) {
        std::cerr << "click-demo: no generated Button to click\n";
        return 3;
    }
    (void)host.begin_frame(0.0);

    const auto physical = static_cast<std::uint32_t>(static_cast<float>(args.width));
    const auto physical_height = static_cast<std::uint32_t>(static_cast<float>(args.height));
    const rhi::Extent2D extent{physical, physical_height};
    const rhi::RenderTargetDesc desc = target_desc(extent, format);
    auto target = device->create_render_target(desc);
    auto commands = device->create_command_list();
    renderer.begin(*commands, *target, desc);
    sink.begin_text_frame();
    tessera::ui::paint::paint_scene(scene.root, host.styles(), sink);
    renderer.end();
    commands->submit();

    const std::size_t bytes = static_cast<std::size_t>(extent.width) * extent.height * 4;
    rhi::BufferDesc readback_desc;
    readback_desc.size = bytes;
    readback_desc.usage = rhi::BufferUsage::Readback;
    auto readback = device->create_buffer(readback_desc);
    auto copy = device->create_command_list();
    copy->copy_target_to_buffer(*target, *readback);
    copy->submit();
    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());

    if (!write_png(args.capture, extent, pixels)) {
        std::cerr << "click-demo: failed to write " << args.capture << "\n";
        return 4;
    }
    std::cout << "click-demo: captured " << args.capture << " (" << extent.width << "x"
              << extent.height << ", click=" << (args.click ? "yes" : "no")
              << ") draws=" << renderer.draw_calls() << "\n";
    return 0;
}

int run_window(const Args& args) {
    tessera::hal::WindowDesc window_desc;
    window_desc.title = "Tessera click-demo";
    window_desc.width = static_cast<std::uint32_t>(args.width);
    window_desc.height = static_cast<std::uint32_t>(args.height);
    window_desc.visible = !args.hidden;
    auto window = tessera::hal::create_window(window_desc);
    if (window == nullptr) {
        std::cout << "click-demo: skipped (no platform window backend)\n";
        return 0;
    }
    auto device = create_device();
    if (device == nullptr) {
        return 0;
    }

    const tessera::hal::WindowMetrics initial = window->metrics();
    rhi::SwapchainDesc swap_desc;
    swap_desc.extent = {initial.physical_width, initial.physical_height};
    swap_desc.image_count = 2;
    swap_desc.vsync = true;
    swap_desc.native_window = window->native_handle().window;
    swap_desc.native_display = window->native_handle().display;
    auto created = device->create_swapchain(swap_desc);
    if (!created) {
        std::cout << "click-demo: skipped (presentation surface unavailable)\n";
        return 0;
    }
    std::unique_ptr<rhi::Swapchain> swapchain = std::move(*created);

    auto pipeline_desc = tessera::rendering::vulkan::batched2d_pipeline_desc();
    pipeline_desc.color_format = swapchain->format();
    auto pipeline = device->create_pipeline(pipeline_desc);
    auto white = create_white(*device);
    constexpr std::size_t kFramesInFlight = 2;
    gpu::Batched2DConfig renderer_config{4096, true, false};
    renderer_config.frames_in_flight = static_cast<std::uint32_t>(kFramesInFlight);
    gpu::Batched2DRenderer renderer(*device, *pipeline, *white, renderer_config);
    TextStack text(*device, args.font);
    tessera::ui::paint::Batched2DSink sink(renderer, &text.renderer, &text.shaper);

    click_demo::DemoScene scene;
    add_root_style(scene.root);

    tessera::runtime::SteadyClock clock;
    tessera::runtime::RuntimeLoop loop(clock);
    tessera::app::HostConfig config;
    config.width = static_cast<float>(initial.logical_width);
    config.height = static_cast<float>(initial.logical_height);
    config.dpi_scale = initial.dpi_scale;
    tessera::app::AppHost host(scene.root, click_demo::demo_stylesheet(), loop, config);
    host.set_update_scheduler(&scene.scheduler);

    tessera::ui::InputPump pump;
    window->set_input_queue(&pump.queue());

    std::array<std::unique_ptr<rhi::CommandList>, kFramesInFlight> in_flight;
    std::size_t slot = 0;
    constexpr std::uint32_t kSlotWaitMs = 1000;

    bool closed = false;
    std::uint32_t rendered = 0;
    std::uint32_t presented = 0;
    double last_tick = tessera::hal::steady_now_seconds();
    std::array<tessera::hal::WindowEvent, 16> events{};

    // The first frame is caused by startup: the scene has no dirty flags until the first layout, so
    // the render-on-demand loop needs an explicit cause before it idles.
    loop.mark_render_dirty();

    while (!closed) {
        const std::size_t event_count = window->poll_events(events);
        for (std::size_t index = 0; index < event_count; ++index) {
            switch (events[index].type) {
            case tessera::hal::WindowEventType::Close:
                closed = true;
                break;
            case tessera::hal::WindowEventType::Resize:
            case tessera::hal::WindowEventType::DpiChange: {
                const tessera::hal::WindowMetrics metrics = window->metrics();
                swapchain->resize({metrics.physical_width, metrics.physical_height});
                host.resize(static_cast<float>(metrics.logical_width),
                            static_cast<float>(metrics.logical_height), metrics.dpi_scale);
                loop.mark_render_dirty();
                break;
            }
            default:
                break;
            }
        }
        if (closed) {
            break;
        }

        (void)pump.pump(scene.root, loop);
        if (scene.root.subtree_dirty() != tessera::ui::Dirty::Clean ||
            scene.root.dirty() != tessera::ui::Dirty::Clean) {
            loop.mark_render_dirty();
        }
        // `--frames N` is the CI smoke mode: it asks the render-on-demand loop for N frames even
        // though a static UI would correctly idle after the first one.
        const bool smoke_frames = args.frames > 0;
        if (smoke_frames && rendered < static_cast<std::uint32_t>(args.frames)) {
            loop.mark_render_dirty();
        }

        const tessera::runtime::RuntimeTick tick = loop.tick();
        if (!tick.should_render) {
            window->wait_for_events(0.25);
            continue;
        }

        const double now = tessera::hal::steady_now_seconds();
        const double dt = now - last_tick;
        last_tick = now;
        const tessera::app::HostFrame frame = host.begin_frame(dt, tick.alpha);
        if (!frame.frame_due && !smoke_frames) {
            loop.clear_render_dirty();
            window->wait_for_events(0.02);
            continue;
        }

        if (in_flight[slot] && !in_flight[slot]->wait(kSlotWaitMs)) {
            continue;
        }
        in_flight[slot].reset();

        std::optional<std::uint32_t> image;
        for (int attempt = 0; attempt < 4 && !image; ++attempt) {
            const auto acquired = swapchain->acquire_next_image(100);
            if (acquired) {
                image = *acquired;
                break;
            }
            if (acquired.error() == rhi::SwapchainError::OutOfDate) {
                swapchain->resize(swapchain->extent());
                continue;
            }
            break;
        }
        if (!image) {
            break;
        }
        rhi::RenderTarget* target = swapchain->image_target(*image);
        if (target == nullptr) {
            break;
        }
        const rhi::RenderTargetDesc desc = target_desc(swapchain->extent(), swapchain->format());
        auto commands = device->create_command_list();
        renderer.begin(*commands, *target, desc);
        sink.begin_text_frame();
        // Swapchain images are not preserved across presents, so every presented frame is a full
        // redraw; the damage plan decided above whether this frame happens at all.
        tessera::ui::paint::paint_scene(scene.root, host.styles(), sink);
        renderer.end();
        commands->submit_async();
        in_flight[slot] = std::move(commands);
        slot = (slot + 1) % kFramesInFlight;
        ++rendered;

        const auto present = swapchain->present(*image);
        if (present || present.error() == rhi::SwapchainError::Suboptimal) {
            ++presented;
        }
        host.end_frame();
        loop.clear_render_dirty();
        if (args.frames > 0 && rendered >= static_cast<std::uint32_t>(args.frames)) {
            break;
        }
        window->wait_for_events(0.001);
    }

    window->set_input_queue(nullptr);
    device->wait_idle();
    std::cout << "click-demo: rendered=" << rendered << " presented=" << presented
              << " hidden=" << (args.hidden ? "yes" : "no") << "\n";
    if (args.frames > 0 && rendered == 0) {
        std::cerr << "click-demo: no frame was rendered\n";
        return 1;
    }
    return 0;
}

Args parse_args(int argc, char** argv) {
    Args args;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--capture" && index + 1 < argc) {
            args.capture = argv[++index];
        } else if (arg == "--font" && index + 1 < argc) {
            args.font = argv[++index];
        } else if (arg == "--width" && index + 1 < argc) {
            args.width = std::stoi(argv[++index]);
        } else if (arg == "--height" && index + 1 < argc) {
            args.height = std::stoi(argv[++index]);
        } else if (arg == "--frames" && index + 1 < argc) {
            args.frames = std::stoi(argv[++index]);
        } else if (arg == "--click") {
            args.click = true;
        } else if (arg == "--windowed") {
            args.windowed = true;
        } else if (arg == "--hidden") {
            args.hidden = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: click_demo [--capture out.png] [--click] [--windowed] [--hidden]\n"
                         "                 [--frames N] [--width N] [--height N] [--font path]\n";
            std::exit(0);
        } else {
            std::cerr << "click-demo: unknown argument " << arg << "\n";
            std::exit(2);
        }
    }
    return args;
}

} // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    if (!args.capture.empty()) {
        return run_capture(args);
    }
    return run_window(args);
}
