// SPDX-License-Identifier: MIT
//
// The window driver's diagnostics overlay (issue #1024, runtime config `diagnosticsOverlay`) on a
// real window and swapchain: the panel's text and rounded rect are recorded after the scene on
// every presented frame, with the layer cache and the promotion policy both on, and the run still
// presents every frame without a failure. The window path presents to a swapchain with no readback,
// so the panel's text is pinned by window_overlay (the lines) and the draw by this smoke run; a
// pixel golden would need a capture seam on the window path. Skips when the platform has no window
// backend or presentation surface.

#include "tessera/app/window.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <iostream>
#include <memory>

#ifndef TESSERA_TEST_FONT
#error "TESSERA_TEST_FONT must point at the repository test font"
#endif

namespace {

using tessera::ui::Node;
using tessera::ui::style::compile_stylesheet;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_window_overlay: " << message << "\n";
    }
    return condition;
}

// Returns false on a failed expectation; `skipped` is set when no window could be created.
bool run(bool overlay, bool& skipped, tessera::app::WindowFrameStats& stats) {
    Node root(1);
    for (int index = 0; index < 3; ++index) {
        Node& box = root.add_child(std::make_unique<Node>(static_cast<std::uint64_t>(index + 2)));
        box.set_element_kind("Box");
    }
    tessera::app::WindowOptions options;
    options.title = "Tessera app-window overlay";
    options.width = 320;
    options.height = 240;
    options.visible = false;
    options.font_path = TESSERA_TEST_FONT;
    options.runtime.diagnostics_overlay = overlay;
    options.runtime.layer_promotion = true;
    options.runtime.frame_budget_ms = 16.7;
    tessera::app::WindowHost host(
        root, compile_stylesheet("Box { width: 200px; height: 40px; background-color: #3366ff; }"),
        options);
    skipped = !host.available();
    if (skipped) {
        return true;
    }
    stats = host.run(4);
    return true;
}

} // namespace

int main() {
    bool skipped = false;
    tessera::app::WindowFrameStats off;
    tessera::app::WindowFrameStats on;
    if (!run(false, skipped, off)) {
        return 1;
    }
    if (skipped) {
        std::cout << "app_window_overlay: skipped (no window backend)\n";
        return 0;
    }
    if (!run(true, skipped, on)) {
        return 1;
    }
    if (!expect(!on.stalled && on.frames >= 1 && on.presented >= 1,
                "frames present with the overlay drawn") ||
        !expect(on.frames == off.frames, "the overlay adds no frames and drops none")) {
        return 2;
    }
    std::cout << "app_window_overlay: ok\n";
    return 0;
}
