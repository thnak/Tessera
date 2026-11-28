// SPDX-License-Identifier: MIT
//
// Weight-selected faces through the real capture path (issue #973): a label with `font-weight: 700`
// paints with the bold face when `CaptureOptions::extra_font_paths` names one, differs from the
// regular paint, and a 400 label is unchanged by the extra face. Skips when the machine has no
// Vulkan device.

#include "tessera/app/window.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <iostream>
#include <memory>
#include <string>

#ifndef TESSERA_PLEX_REGULAR
#error "TESSERA_PLEX_REGULAR and TESSERA_PLEX_BOLD must name the font pack faces"
#endif

namespace {

using tessera::ui::Node;
using tessera::ui::PaintData;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_window_font_weight: " << message << "\n";
    }
    return condition;
}

struct Scene {
    Scene() {
        root.set_element_kind("View");
        auto text_ptr = std::make_unique<Node>(2);
        Node& text = root.add_child(std::move(text_ptr));
        text.set_element_kind("Text");
        PaintData data;
        data.kind = tessera::ui::PaintKind::Text;
        data.text = "Save changes";
        text.set_paint_data(std::move(data));
    }
    Node root{1};
};

tessera::ui::style::CompiledStyleSheet sheet(const char* weight) {
    const std::string css =
        std::string("View { width: 240px; padding: 16px; background-color: #ffffff; }\n") +
        "Text { font-size: 16px; color: #000000; font-weight: " + weight + "; }\n";
    return tessera::ui::style::compile_stylesheet(css);
}

tessera::app::CaptureOptions options(bool with_bold) {
    tessera::app::CaptureOptions capture;
    capture.width = 280;
    capture.height = 80;
    capture.font_path = TESSERA_PLEX_REGULAR;
    if (with_bold) {
        capture.extra_font_paths.emplace_back(TESSERA_PLEX_BOLD);
    }
    return capture;
}

} // namespace

int main() {
    Scene scene;
    const auto regular_alone =
        tessera::app::capture_frame(scene.root, sheet("400"), options(false));
    if (!regular_alone) {
        std::cout << "app_window_font_weight: skipped (Vulkan device unavailable)\n";
        return 0;
    }
    const auto regular_with_bold =
        tessera::app::capture_frame(scene.root, sheet("400"), options(true));
    const auto bold_alone = tessera::app::capture_frame(scene.root, sheet("700"), options(false));
    const auto bold_with_bold =
        tessera::app::capture_frame(scene.root, sheet("700"), options(true));
    if (!regular_with_bold || !bold_alone || !bold_with_bold) {
        return 1;
    }
    if (!expect(regular_with_bold->rgba == regular_alone->rgba,
                "a 400 label ignores the extra bold face") ||
        !expect(bold_alone->rgba == regular_alone->rgba,
                "with one face a 700 label draws the regular glyphs") ||
        !expect(bold_with_bold->rgba != regular_alone->rgba,
                "with a bold face a 700 label paints differently")) {
        return 2;
    }
    return 0;
}
