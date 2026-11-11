// SPDX-License-Identifier: MIT
//
// App-window capture contract: `capture_frame` paints the AppHost scene through the Batched2D sink
// into an offscreen target and reads it back. The same host state must produce identical pixels,
// and a state change made in `prepare` must change them. Skips when the machine has no Vulkan
// device (the capture function reports that as nullopt instead of failing).

#include "tessera/app/window.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#ifndef TESSERA_TEST_FONT
#error "TESSERA_TEST_FONT must point at the repository test font"
#endif

namespace {

using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::style::compile_stylesheet;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_window_capture: " << message << "\n";
    }
    return condition;
}

std::uint64_t fnv1a(std::span<const std::uint8_t> bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

tessera::ui::style::CompiledStyleSheet sheet() {
    return compile_stylesheet(R"(
View { width: 240px; padding: 16px; border-radius: 12px; background-color: #1e293b; }
Text { font-size: 16px; color: #e2e8f0; margin-bottom: 8px; }
)");
}

struct Scene {
    Scene() {
        root.set_element_kind("View");
        auto card_ptr = std::make_unique<Node>(2);
        Node& card = root.add_child(std::move(card_ptr));
        card.set_element_kind("View");
        auto text_ptr = std::make_unique<Node>(3);
        Node& text = card.add_child(std::move(text_ptr));
        text.set_element_kind("Text");
        text.set_paint_data(payload("Count: 0"));
        label = &text;
    }

    static PaintData payload(std::string value) {
        PaintData data;
        data.kind = tessera::ui::PaintKind::Text;
        data.text = std::move(value);
        return data;
    }

    Node root{1};
    Node* label = nullptr;
};

tessera::app::CaptureOptions options() {
    tessera::app::CaptureOptions capture;
    capture.width = 320;
    capture.height = 200;
    capture.font_path = TESSERA_TEST_FONT;
    return capture;
}

} // namespace

int main() {
    Scene scene;
    const auto before = tessera::app::capture_frame(scene.root, sheet(), options());
    if (!before) {
        std::cout << "app_window_capture: skipped (Vulkan device unavailable)\n";
        return 0;
    }
    if (!expect(before->width == 320 && before->height == 200, "capture extent matches options") ||
        !expect(before->rgba.size() == 320u * 200u * 4u, "capture buffer is RGBA8") ||
        !expect(fnv1a(before->rgba) != 0, "capture has pixels")) {
        return 1;
    }

    // Painted content: the card background differs from the clear color somewhere.
    bool painted = false;
    for (std::size_t index = 0; index < before->rgba.size(); index += 4) {
        if (before->rgba[index] != 11 || before->rgba[index + 1] != 18 ||
            before->rgba[index + 2] != 32) {
            painted = true;
            break;
        }
    }
    if (!expect(painted, "the scene painted over the clear color")) {
        return 2;
    }

    // Deterministic for the same scene state.
    const auto again = tessera::app::capture_frame(scene.root, sheet(), options());
    if (!again) {
        return 3;
    }
    if (!expect(again->rgba == before->rgba, "two captures of the same state are identical")) {
        return 3;
    }

    // A state change in `prepare` (after the first layout, before the captured frame) is painted.
    const auto after = tessera::app::capture_frame(scene.root, sheet(), options(), [&] {
        scene.label->set_paint_data(Scene::payload("Count: 7"));
    });
    if (!after) {
        return 4;
    }
    if (!expect(after->rgba != before->rgba, "a label state change repaints the capture") ||
        !expect(fnv1a(after->rgba) != fnv1a(before->rgba), "the changed capture hashes differ")) {
        return 4;
    }

    std::cout << "app_window_capture: ok\n";
    return 0;
}
