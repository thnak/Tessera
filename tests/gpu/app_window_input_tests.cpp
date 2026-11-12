// SPDX-License-Identifier: MIT
//
// Window-path input: a real hidden platform window + Vulkan swapchain (WindowHost), generated Demo
// component, and normalized input events posted through `WindowHost::post_input`, which the host
// pumps through the same InputPump/InputRouter path as OS input. Proves pointer focus, click ->
// generated handler -> state and typed text -> generated onChange on the window path. Skips when
// the platform has no window backend or presentation surface.

#include "Demo.hpp"
#include "tessera/app/window.hpp"
#include "tessera/ui/input.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#ifndef TESSERA_TEST_FONT
#error "TESSERA_TEST_FONT must point at the repository test font"
#endif

namespace {

using tessera::rendering::rhi::Rect;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::Node;
using tessera::ui::PaintKind;
using tessera::ui::style::compile_stylesheet;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_window_input: " << message << "\n";
    }
    return condition;
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

InputEvent pointer(InputEventType type, float x, float y) {
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

InputEvent typed_text(std::string_view text) {
    InputEvent event;
    event.type = InputEventType::EditText;
    const std::size_t size = std::min<std::size_t>(text.size(), event.text.size());
    std::copy_n(text.data(), size, event.text.begin());
    event.text_size = static_cast<std::uint32_t>(size);
    return event;
}

void press_release(tessera::app::WindowHost& host, const Rect& box) {
    host.post_input(pointer(InputEventType::PointerDown, box.x + 8.0f, box.y + 8.0f));
    host.post_input(pointer(InputEventType::PointerUp, box.x + 8.0f, box.y + 8.0f));
}

} // namespace

int main() {
    Node root(900);
    demo::gen::Demo demo;
    demo.build(root);
    demo.mount();
    tessera::ui::UpdateScheduler scheduler;
    demo.attach_scheduler(scheduler);

    tessera::app::WindowOptions options;
    options.title = "Tessera app-window input smoke";
    options.width = 360;
    options.height = 260;
    options.visible = false;
    options.font_path = TESSERA_TEST_FONT;

    tessera::app::WindowHost host(root, compile_stylesheet(R"(
View { padding: 16px; background-color: #1e293b; }
Text { font-size: 14px; color: #e2e8f0; margin-bottom: 6px; }
Button { width: 100px; height: 30px; margin-bottom: 8px; background-color: #2563eb; }
TextInput { width: 180px; height: 28px; background-color: #0f172a; }
)"),
                                  options, &scheduler);
    if (!host.available()) {
        std::cout << "app_window_input: skipped (no window backend or presentation surface)\n";
        return 0;
    }

    // First frame lays the generated scene out.
    (void)host.run(1);
    Node* button = find_kind(root, "Button");
    Node* input = find_kind(root, "TextInput");
    if (!expect(button != nullptr && input != nullptr,
                "the generated Button and TextInput exist") ||
        !expect(button->world_bounds().width > 0.0f, "the first frame laid the scene out")) {
        return 1;
    }

    // Click through the window pump: generated handler -> state.
    press_release(host, button->world_bounds());
    (void)host.run(2);
    if (!expect(demo.state_count_ == 1, "the window path click incremented the generated state")) {
        return 2;
    }

    // Press the TextInput to focus it through the pump, then type.
    const auto input_box = input->world_bounds();
    host.post_input(pointer(InputEventType::PointerDown, input_box.x + 8.0f, input_box.y + 8.0f));
    (void)host.run(1);
    host.post_input(typed_text("Ada"));
    (void)host.run(2);
    if (!expect(demo.state_name_ == "Ada", "typed text reached the generated onChange") ||
        !expect(input->paint_data().has_value() && input->paint_data()->kind == PaintKind::Text &&
                    input->paint_data()->text == "Ada",
                "the controlled TextInput paints the typed value") ||
        !expect(input->paint_data()->decoration.has_value() &&
                    input->paint_data()->decoration->caret_byte_offset == 3 &&
                    input->paint_data()->decoration->caret_visible,
                "the window path publishes caret decoration too")) {
        return 3;
    }

    // Resize/DPI through the synthetic metrics seam: same path as an OS resize event.
    tessera::hal::WindowMetrics metrics;
    metrics.logical_width = 480;
    metrics.logical_height = 320;
    metrics.physical_width = 960;
    metrics.physical_height = 640;
    metrics.dpi_scale = 2.0f;
    host.apply_metrics(metrics);
    (void)host.run(2);
    if (!expect(root.world_bounds().width == 480.0f, "the root reflows to the new logical width") ||
        !expect(host.host().width() == 480.0f, "the host reports the new logical width") ||
        !expect(host.host().target_extent().width == 960, "the damage target follows DPI")) {
        return 4;
    }

    std::cout << "app_window_input: ok\n";
    return 0;
}
