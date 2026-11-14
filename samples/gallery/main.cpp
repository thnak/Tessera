// SPDX-License-Identifier: MIT
//
// gallery: the Instrument library gallery skeleton (issue #239, ADR-047 item 10, native lane).
//
// The page is the generated `gallery::gen::Gallery` (samples/gallery/Gallery.tsx, importing the
// library `Button`). The switches are host-side state around it, never page logic:
//   --theme ID         instrument-light (default), instrument-dark or instrument-high-contrast
//   --density D        comfortable (default) or compact: token overrides in gallery.css
//   --text-scale N     100 (default), 125 or 150: token overrides in gallery.css
//   --rtl              `direction: rtl` on the window root (inherited)
//   --state S          none (default), hover, active or focus: forces that pseudo-state on every
//                      enabled Button, so a capture shows the state without a pointer
//
// Modes:
//   gallery --capture out.png     windowless offscreen capture (any Vulkan device, lavapipe in CI)
//   gallery --golden base.png     the same capture compared with a committed lavapipe baseline
//                                 (--artifacts DIR keeps frame/diff/report, --update-goldens
//                                 rewrites)
//   gallery [--hidden] [--frames N]  window; the switches above set its initial state

#include "Gallery.hpp"
#include "golden.hpp"
#include "stb_image_write.h"
#include "tessera/app/window.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/prepared_stylesheet.hpp"
#include "tessera/ui/style/pseudo_state.hpp"
#include "tessera/ui/style/theme_asset.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef TESSERA_GALLERY_FONT
#define TESSERA_GALLERY_FONT "tests/fixtures/fonts/NotoSans-Regular.ttf"
#endif

namespace {

namespace style = tessera::ui::style;
using tessera::ui::Node;

struct Args {
    std::string capture;
    std::string golden;
    std::string artifact_dir = "gallery-artifacts";
    bool update_goldens = false;
    std::string font = TESSERA_GALLERY_FONT;
    std::string theme = "instrument-light";
    std::string density = "comfortable";
    std::string state = "none";
    int text_scale = 100;
    bool rtl = false;
    bool hidden = false;
    int frames = 0;
    int width = 520;
    int height = 300;
};

// The scene root is the window's content node; the id is outside the generated id range.
constexpr tessera::ui::NodeId kRootId = 900;

std::optional<std::string> read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Everything the page needs besides the generated component: the library Button sheet (layered in
// `tessera-ui`), the gallery sheet, and the theme asset blob. Built once from the repository files.
struct Resources {
    style::PreparedStyleSheet library_sheet;
    style::PreparedStyleSheet gallery_sheet;
    std::vector<std::uint8_t> theme_blob;
};

std::optional<Resources> load_resources() {
    const auto button_css = read_file(TESSERA_GALLERY_BUTTON_CSS);
    const auto gallery_css = read_file(TESSERA_GALLERY_CSS);
    const auto theme_json = read_file(TESSERA_GALLERY_THEME);
    if (!button_css || !gallery_css || !theme_json) {
        std::cerr << "gallery: cannot read the library CSS, gallery CSS or theme asset\n";
        return std::nullopt;
    }
    const auto theme = style::parse_theme_asset(*theme_json);
    if (!theme) {
        std::cerr << "gallery: theme asset: " << theme.error().message << "\n";
        return std::nullopt;
    }
    Resources resources;
    resources.library_sheet = style::prepare_stylesheet(*button_css);
    resources.gallery_sheet = style::prepare_stylesheet(*gallery_css);
    resources.theme_blob = style::serialize_theme_asset(*theme);
    return resources;
}

style::CompiledStyleSheet make_sheet(const Resources& resources) {
    const std::vector<style::StylesheetSource> sources{
        {"button.css", &resources.library_sheet, style::Origin::Author, true},
        {"gallery.css", &resources.gallery_sheet, style::Origin::Author, false},
    };
    return style::CompiledStyleSheet(sources);
}

std::string root_classes(const Args& args) {
    std::string classes = "g-root";
    if (args.density == "compact") {
        classes += " g-compact";
    }
    if (args.rtl) {
        classes += " g-rtl";
    }
    if (args.text_scale == 125) {
        classes += " g-scale-125";
    } else if (args.text_scale == 150) {
        classes += " g-scale-150";
    }
    return classes;
}

void collect_buttons(Node& node, std::vector<Node*>& out) {
    if (node.role() == "button" && node.focusable() &&
        !style::has_state(node.pseudo_state(), style::PseudoState::Disabled) &&
        node.class_name().find("tbtn-disabled") == std::string_view::npos) {
        out.push_back(&node);
    }
    for (const auto& child : node.children()) {
        collect_buttons(*child, out);
    }
}

// Forces the requested pseudo-state on every enabled Button; the stylesheet rules are the ones a
// real pointer or keyboard focus would trigger.
void apply_state(Node& root, std::string_view state) {
    style::PseudoState bits = style::PseudoState::None;
    if (state == "hover") {
        bits = style::PseudoState::Hover;
    } else if (state == "active") {
        bits = style::PseudoState::Hover | style::PseudoState::Active;
    } else if (state == "focus") {
        bits = style::PseudoState::Focus | style::PseudoState::FocusVisible;
    }
    if (bits == style::PseudoState::None) {
        return;
    }
    std::vector<Node*> buttons;
    collect_buttons(root, buttons);
    for (Node* button : buttons) {
        button->set_pseudo_state(bits, true);
    }
}

void configure(tessera::app::AppHost& host, const Resources& resources, const Args& args) {
    host.register_embedded_theme(resources.theme_blob);
    if (!host.set_theme(args.theme)) {
        std::cerr << "gallery: unknown theme `" << args.theme << "`\n";
        std::exit(2);
    }
}

bool write_png(const std::string& path, const tessera::app::CapturedFrame& frame) {
    return stbi_write_png(path.c_str(), static_cast<int>(frame.width),
                          static_cast<int>(frame.height), 4, frame.rgba.data(),
                          static_cast<int>(frame.width) * 4) != 0;
}

struct Scene {
    Scene() {
        gallery.build(root);
        gallery.mount();
        gallery.attach_scheduler(scheduler);
    }

    Node root{kRootId};
    gallery::gen::Gallery gallery;
    tessera::ui::UpdateScheduler scheduler;
};

int run_capture(const Args& args, const Resources& resources, Scene& scene) {
    std::string unavailable;
    if (!args.golden.empty() && !gallery_golden::available(unavailable)) {
        std::cout << "gallery: skipped golden (" << unavailable << ")\n";
        return 0;
    }
    tessera::app::CaptureOptions options;
    options.width = static_cast<std::uint32_t>(args.width);
    options.height = static_cast<std::uint32_t>(args.height);
    options.font_path = args.font;
    options.scheduler = &scene.scheduler;
    options.configure_host = [&](tessera::app::AppHost& host) { configure(host, resources, args); };
    const std::optional<tessera::app::CapturedFrame> frame = tessera::app::capture_frame(
        scene.root, make_sheet(resources), options, [&] { apply_state(scene.root, args.state); });
    if (!frame) {
        std::cout << "gallery: skipped (Vulkan device unavailable)\n";
        return 0;
    }
    if (!args.golden.empty()) {
        return gallery_golden::check(*frame, args.golden, args.artifact_dir, args.update_goldens);
    }
    if (!write_png(args.capture, *frame)) {
        std::cerr << "gallery: failed to write " << args.capture << "\n";
        return 4;
    }
    std::cout << "gallery: captured " << args.capture << " (" << frame->width << "x"
              << frame->height << ", theme=" << args.theme << ", density=" << args.density
              << ", text-scale=" << args.text_scale << ", rtl=" << (args.rtl ? "yes" : "no")
              << ", state=" << args.state << ")\n";
    return 0;
}

int run_window(const Args& args, const Resources& resources, Scene& scene) {
    tessera::app::WindowOptions options;
    options.title = "Tessera gallery";
    options.width = static_cast<std::uint32_t>(args.width);
    options.height = static_cast<std::uint32_t>(args.height);
    options.visible = !args.hidden;
    options.vsync = true;
    options.font_path = args.font;
    tessera::app::WindowHost host(scene.root, make_sheet(resources), options, &scene.scheduler);
    if (!host.available()) {
        std::cout << "gallery: skipped (no window backend or presentation surface)\n";
        return 0;
    }
    configure(host.host(), resources, args);
    apply_state(scene.root, args.state);
    const tessera::app::WindowFrameStats stats = host.run(static_cast<std::uint32_t>(args.frames));
    std::cout << "gallery: frames=" << stats.frames << " presented=" << stats.presented
              << " idle_waits=" << stats.idle_waits << " input=" << stats.input_events << "\n";
    return args.frames > 0 && stats.frames == 0 ? 1 : 0;
}

[[noreturn]] void usage(int code) {
    std::cout
        << "usage: gallery [--capture out.png] [--theme ID] [--density comfortable|compact]\n"
           "               [--text-scale 100|125|150] [--rtl] [--state none|hover|active|focus]\n"
           "               [--hidden] [--frames N] [--width N] [--height N] [--font path]\n";
    std::exit(code);
}

Args parse_args(int argc, char** argv) {
    Args args;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        const bool has_value = index + 1 < argc;
        if (arg == "--capture" && has_value) {
            args.capture = argv[++index];
        } else if (arg == "--golden" && has_value) {
            args.golden = argv[++index];
            args.capture = "golden";
        } else if (arg == "--artifacts" && has_value) {
            args.artifact_dir = argv[++index];
        } else if (arg == "--update-goldens") {
            args.update_goldens = true;
        } else if (arg == "--theme" && has_value) {
            args.theme = argv[++index];
        } else if (arg == "--density" && has_value) {
            args.density = argv[++index];
        } else if (arg == "--state" && has_value) {
            args.state = argv[++index];
        } else if (arg == "--text-scale" && has_value) {
            args.text_scale = std::stoi(argv[++index]);
        } else if (arg == "--font" && has_value) {
            args.font = argv[++index];
        } else if (arg == "--width" && has_value) {
            args.width = std::stoi(argv[++index]);
        } else if (arg == "--height" && has_value) {
            args.height = std::stoi(argv[++index]);
        } else if (arg == "--frames" && has_value) {
            args.frames = std::stoi(argv[++index]);
        } else if (arg == "--rtl") {
            args.rtl = true;
        } else if (arg == "--hidden") {
            args.hidden = true;
        } else if (arg == "--help" || arg == "-h") {
            usage(0);
        } else {
            std::cerr << "gallery: unknown argument " << arg << "\n";
            usage(2);
        }
    }
    const bool density_ok = args.density == "comfortable" || args.density == "compact";
    const bool scale_ok =
        args.text_scale == 100 || args.text_scale == 125 || args.text_scale == 150;
    const bool state_ok = args.state == "none" || args.state == "hover" || args.state == "active" ||
                          args.state == "focus";
    if (!density_ok || !scale_ok || !state_ok) {
        std::cerr << "gallery: invalid --density, --text-scale or --state\n";
        usage(2);
    }
    return args;
}

} // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    const std::optional<Resources> resources = load_resources();
    if (!resources) {
        return 3;
    }
    const std::string classes = root_classes(args);
    Scene scene;
    // `Node` keeps a view of its class name, so the text outlives the scene: `classes` is owned
    // here.
    scene.root.set_class_name(classes);
    if (!args.capture.empty()) {
        return run_capture(args, *resources, scene);
    }
    return run_window(args, *resources, scene);
}
