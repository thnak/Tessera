// SPDX-License-Identifier: MIT
//
// App-window smoke: the WindowHost creates a hidden platform window plus a Vulkan swapchain, runs
// the AppHost loop and presents N forced frames. Skips when the platform has no window backend or
// no presentation surface (the host reports available() == false instead of failing).

#include "tessera/app/window.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <utility>

#ifndef TESSERA_TEST_FONT
#error "TESSERA_TEST_FONT must point at the repository test font"
#endif

namespace {

using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::style::compile_stylesheet;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_window_smoke: " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    Node root(1);
    auto label_ptr = std::make_unique<Node>(2);
    Node& label = root.add_child(std::move(label_ptr));
    label.set_element_kind("Text");
    PaintData data;
    data.kind = tessera::ui::PaintKind::Text;
    data.text = "App window smoke";
    label.set_paint_data(std::move(data));

    tessera::app::WindowOptions options;
    options.title = "Tessera app-window smoke";
    options.width = 320;
    options.height = 200;
    options.visible = false;
    options.font_path = TESSERA_TEST_FONT;

    tessera::app::WindowHost host(root, compile_stylesheet("Text { font-size: 16px; }"), options);
    if (!host.available()) {
        std::cout << "app_window_smoke: skipped (no window backend or presentation surface)\n";
        return 0;
    }

    const tessera::app::WindowFrameStats stats = host.run(3);
    if (!expect(stats.frames >= 3, "the loop painted at least three frames") ||
        !expect(stats.presented >= 3, "the loop presented at least three frames")) {
        return 1;
    }

    // run() is re-entrant: the input queue is reattached on entry and teardown drains the
    // submitted lists without a device-wide wait, so a second bounded run still produces frames.
    const tessera::app::WindowFrameStats second = host.run(2);
    if (!expect(second.frames >= 2, "a second run paints frames") ||
        !expect(second.presented >= 2, "a second run presents frames")) {
        return 2;
    }
    std::cout << "app_window_smoke: ok (frames=" << stats.frames << " presented=" << stats.presented
              << " idle_waits=" << stats.idle_waits << " second_frames=" << second.frames << ")\n";
    return 0;
}
