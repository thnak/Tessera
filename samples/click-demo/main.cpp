// SPDX-License-Identifier: MIT
//
// click-demo: the interactive Tessera UI acceptance for the app-host lane, built on the public
// app-window library.
//
// Scene: the generated `demo::Demo` component (samples/compiler-demo/Demo.tsx -> IR -> C++),
// styled by a real compiled stylesheet, laid out by Yoga, painted through Batched2D.
//
// Modes:
//   click_demo --capture shot.png           windowless offscreen capture (works on any Vulkan
//                                            device, including lavapipe in CI)
//   click_demo --click --capture after.png  dispatches a real pointer click on the generated Button
//                                            through the InputRouter before capturing
//   click_demo --windowed [--frames N]      visible window, real mouse input, present
//   click_demo --windowed --hidden --frames 5  CI smoke: hidden window, five presented frames

#include "scene.hpp"
#include "stb_image_write.h"
#include "tessera/app/window.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifndef TESSERA_SAMPLE_FONT
#define TESSERA_SAMPLE_FONT "tests/fixtures/fonts/NotoSans-Regular.ttf"
#endif

namespace {

namespace rhi = tessera::rendering::rhi;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::Node;

struct Args {
    std::string capture;
    std::string font = TESSERA_SAMPLE_FONT;
    std::string type;
    bool click = false;
    bool windowed = false;
    bool hidden = false;
    int frames = 0;
    int width = 380;
    int height = 260;
};

void add_root_style(Node& root) {
    // Host-owned geometry first (AppHost sets width/height/display/flex-direction); the sample
    // centers the card and paints the window background, which an app stylesheet would do through a
    // `#root` selector once element ids are generated.
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
    event.pointer_id = 0;
    event.pointer_kind = tessera::ui::PointerKind::Mouse;
    event.button = tessera::ui::PointerButton::Left;
    event.is_primary = true;
    return event;
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

// Focuses the generated TextInput with a real pointer press, then commits typed text through the
// router's EditText path, so the capture shows the controlled value plus caret decoration.
bool type_into_text_input(Node& root, std::string_view text) {
    Node* input = find_kind(root, "TextInput");
    if (input == nullptr) {
        return false;
    }
    const rhi::Rect box = input->world_bounds();
    if (box.width <= 0.0f || box.height <= 0.0f) {
        return false;
    }
    tessera::ui::InputRouter router;
    const float x = box.x + box.width * 0.5f;
    const float y = box.y + box.height * 0.5f;
    (void)router.dispatch(root, pointer_event(InputEventType::PointerDown, x, y));
    (void)router.dispatch(root, pointer_event(InputEventType::PointerUp, x, y));
    InputEvent edit;
    edit.type = InputEventType::EditText;
    const std::size_t size = std::min<std::size_t>(text.size(), edit.text.size());
    std::copy_n(text.data(), size, edit.text.begin());
    edit.text_size = static_cast<std::uint32_t>(size);
    (void)router.dispatch(root, edit);
    return size == text.size();
}

bool write_png(const std::string& path, const tessera::app::CapturedFrame& frame) {
    return stbi_write_png(path.c_str(), static_cast<int>(frame.width),
                          static_cast<int>(frame.height), 4, frame.rgba.data(),
                          static_cast<int>(frame.width) * 4) != 0;
}

int run_capture(const Args& args, click_demo::DemoScene& scene) {
    tessera::app::CaptureOptions options;
    options.width = static_cast<std::uint32_t>(args.width);
    options.height = static_cast<std::uint32_t>(args.height);
    options.font_path = args.font;
    options.scheduler = &scene.scheduler;
    const std::optional<tessera::app::CapturedFrame> frame =
        tessera::app::capture_frame(scene.root, click_demo::demo_stylesheet(), options, [&] {
            if (args.click && !click_generated_button(scene.root)) {
                std::cerr << "click-demo: no generated Button to click\n";
            }
            if (!args.type.empty() && !type_into_text_input(scene.root, args.type)) {
                std::cerr << "click-demo: no generated TextInput to type into\n";
            }
        });
    if (!frame) {
        std::cout << "click-demo: skipped (Vulkan device unavailable)\n";
        return 0;
    }
    if (!write_png(args.capture, *frame)) {
        std::cerr << "click-demo: failed to write " << args.capture << "\n";
        return 4;
    }
    std::cout << "click-demo: captured " << args.capture << " (" << frame->width << "x"
              << frame->height << ", click=" << (args.click ? "yes" : "no")
              << ", typed=" << (args.type.empty() ? "no" : "yes") << ")\n";
    return 0;
}

int run_window(const Args& args, click_demo::DemoScene& scene) {
    tessera::app::WindowOptions options;
    options.title = "Tessera click-demo";
    options.width = static_cast<std::uint32_t>(args.width);
    options.height = static_cast<std::uint32_t>(args.height);
    options.visible = !args.hidden;
    options.vsync = true;
    options.font_path = args.font;
    tessera::app::WindowHost host(scene.root, click_demo::demo_stylesheet(), options,
                                  &scene.scheduler);
    if (!host.available()) {
        std::cout << "click-demo: skipped (no window backend or presentation surface)\n";
        return 0;
    }
    const tessera::app::WindowFrameStats stats = host.run(static_cast<std::uint32_t>(args.frames));
    std::cout << "click-demo: frames=" << stats.frames << " presented=" << stats.presented
              << " idle_waits=" << stats.idle_waits << " input=" << stats.input_events
              << " hidden=" << (args.hidden ? "yes" : "no") << "\n";
    if (args.frames > 0 && stats.frames == 0) {
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
        } else if (arg == "--type" && index + 1 < argc) {
            args.type = argv[++index];
        } else if (arg == "--click") {
            args.click = true;
        } else if (arg == "--windowed") {
            args.windowed = true;
        } else if (arg == "--hidden") {
            args.hidden = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: click_demo [--capture out.png] [--click] [--type TEXT]\n"
                         "                 [--windowed] [--hidden] [--frames N] [--width N]\n"
                         "                 [--height N] [--font path]\n";
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
    click_demo::DemoScene scene;
    add_root_style(scene.root);
    if (!args.capture.empty()) {
        return run_capture(args, scene);
    }
    return run_window(args, scene);
}
