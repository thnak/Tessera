// SPDX-License-Identifier: MIT
//
// A filtered card in a real window (issue #617): the WindowHost creates a hidden platform window
// plus a Vulkan swapchain and presents frames of a tree whose card carries `filter: blur()` and
// `filter: drop-shadow()`. A filter-only tree takes the direct path (no offscreen frame target), so
// its layer passes reopen the swapchain image with a Load pass; the null backend and lavapipe cover
// that with counters, and this test is the one that runs it against a presentable image. It must
// present every frame, never stall or recover a device, and the same scene captured offscreen must
// really be blurred (so the test cannot pass on a scene whose filter was silently dropped). Skips
// when the platform has no window backend or presentation surface.

#include "tessera/app/window.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstddef>
#include <iostream>
#include <memory>
#include <utility>

#ifndef TESSERA_TEST_FONT
#error "TESSERA_TEST_FONT must point at the repository test font"
#endif

namespace {

using tessera::ui::Node;
using tessera::ui::style::compile_stylesheet;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_window_filter: " << message << "\n";
    }
    return condition;
}

tessera::ui::style::CompiledStyleSheet sheet() {
    return compile_stylesheet(R"(
View { padding: 24px; background-color: #0b1220; }
Blurred { width: 120px; height: 80px; margin-bottom: 24px; background-color: #e11d48;
          filter: blur(6px); }
Shadowed { width: 120px; height: 80px; background-color: #e11d48;
           filter: drop-shadow(rgba(0, 0, 255, 0.6) 8px 6px 3px); }
)");
}

struct Scene {
    Scene() {
        root.set_element_kind("View");
        for (const char* kind : {"Blurred", "Shadowed"}) {
            auto child = std::make_unique<Node>(next_id++);
            child->set_element_kind(kind);
            root.add_child(std::move(child));
        }
    }

    Node root{1};
    tessera::ui::NodeId next_id = 2;
};

// Pixels whose red channel sits strictly between the clear colour and the card colour: a hard-edged
// card has none of them in this scene except where its own corners are anti-aliased.
std::size_t blurred_pixels(const tessera::app::CapturedFrame& frame) {
    std::size_t count = 0;
    for (std::size_t index = 0; index + 3 < frame.rgba.size(); index += 4) {
        const unsigned red = frame.rgba[index];
        if (red > 40 && red < 200) {
            ++count;
        }
    }
    return count;
}

} // namespace

int main() {
    Scene scene;

    tessera::app::WindowOptions options;
    options.title = "Tessera app-window filter";
    options.width = 320;
    options.height = 280;
    options.visible = false;
    options.font_path = TESSERA_TEST_FONT;

    tessera::app::WindowHost host(scene.root, sheet(), options);
    if (!host.available()) {
        std::cout << "app_window_filter: skipped (no window backend or presentation surface)\n";
        return 0;
    }

    const tessera::app::WindowFrameStats stats = host.run(6);
    if (!expect(!stats.stalled, "the loop did not give up on repeated frame failures") ||
        !expect(stats.frames >= 6, "the loop painted at least six frames") ||
        !expect(stats.presented >= 6, "the loop presented at least six frames") ||
        !expect(stats.recovered == 0, "no device was recovered while painting a filtered tree")) {
        return 1;
    }

    // A second bounded run re-enters the loop with the layer pool and the swapchain already warm.
    const tessera::app::WindowFrameStats second = host.run(3);
    if (!expect(!second.stalled && second.presented >= 3, "a second run presents frames")) {
        return 2;
    }

    tessera::app::CaptureOptions capture;
    capture.width = 320;
    capture.height = 280;
    capture.font_path = TESSERA_TEST_FONT;
    Scene captured;
    const auto frame = tessera::app::capture_frame(captured.root, sheet(), capture);
    if (!frame) {
        std::cout << "app_window_filter: skipped (Vulkan capture unavailable)\n";
        return 0;
    }
    if (!expect(blurred_pixels(*frame) > 200,
                "the scene is really blurred, so the window test is not vacuous")) {
        return 3;
    }

    std::cout << "app_window_filter: ok (frames=" << stats.frames
              << " presented=" << stats.presented << " second=" << second.presented << ")\n";
    return 0;
}
