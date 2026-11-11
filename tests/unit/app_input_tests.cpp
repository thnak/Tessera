// SPDX-License-Identifier: MIT
//
// Input through the window pump: `InputPump` drains a bounded queue, dispatches through the scene
// InputRouter (hover chain, :active, focus) and hands the result to the AppHost frame. This test
// drives the generated Demo component through exactly that path: pointer move -> :hover paint,
// press -> :active, release -> generated Click handler -> state, focus + EditText -> generated
// onChange -> controlled text.

#include "Demo.hpp"
#include "tessera/app/host.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/input_pump.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::Node;
using tessera::ui::PaintKind;
using tessera::ui::style::compile_stylesheet;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_input: " << message << "\n";
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

bool same_color(const Color& lhs, const Color& rhs) {
    const auto near = [](float a, float b) { return std::fabs(a - b) <= 0.02f; };
    return near(lhs.r, rhs.r) && near(lhs.g, rhs.g) && near(lhs.b, rhs.b);
}

// Records the flat fills so the test can read the button's painted background for a state.
class FillSink final : public tessera::ui::paint::PaintSink {
public:
    struct Fill {
        Rect rect;
        Color color;
    };

    void fill_rect(const Rect& rect, const Color& color) override {
        fills.push_back({rect, color});
    }
    void fill_rounded_rect(const Rect& rect, const Color& color, float /*radius*/) override {
        fills.push_back({rect, color});
    }

    // The smallest fill covering the whole button box (the button background).
    [[nodiscard]] std::optional<Color> at(const Rect& box) const {
        std::optional<Color> found;
        float area = 0.0f;
        for (const Fill& fill : fills) {
            if (fill.rect.x > box.x + 0.5f || fill.rect.y > box.y + 0.5f ||
                fill.rect.x + fill.rect.width < box.x + box.width - 0.5f ||
                fill.rect.y + fill.rect.height < box.y + box.height - 0.5f) {
                continue;
            }
            const float candidate = fill.rect.width * fill.rect.height;
            if (!found || candidate < area) {
                found = fill.color;
                area = candidate;
            }
        }
        return found;
    }

    std::vector<Fill> fills;
};

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

InputEvent edit_text(std::string_view text) {
    InputEvent event;
    event.type = InputEventType::EditText;
    const std::size_t size = std::min<std::size_t>(text.size(), event.text.size());
    std::copy_n(text.data(), size, event.text.begin());
    event.text_size = static_cast<std::uint32_t>(size);
    return event;
}

constexpr Color kDefault = {37.0f / 255.0f, 99.0f / 255.0f, 235.0f / 255.0f, 1.0f};
constexpr Color kHover = {249.0f / 255.0f, 115.0f / 255.0f, 22.0f / 255.0f, 1.0f};
constexpr Color kActive = {220.0f / 255.0f, 38.0f / 255.0f, 38.0f / 255.0f, 1.0f};

std::optional<Color> paint_button(tessera::app::AppHost& host, Node& root, const Rect& box) {
    FillSink sink;
    tessera::ui::paint::paint_scene(root, host.styles(), sink);
    return sink.at(box);
}

} // namespace

int main() {
    Node root(900);
    demo::gen::Demo demo;
    demo.build(root);
    demo.mount();
    tessera::ui::UpdateScheduler scheduler;
    demo.attach_scheduler(scheduler);

    const auto sheet = compile_stylesheet(R"(
View { width: 240px; padding: 16px; background-color: #1e293b; }
Text { font-size: 16px; color: #e2e8f0; }
Button { width: 120px; height: 32px; background-color: #2563eb; }
Button:hover { background-color: #f97316; }
Button:active { background-color: #dc2626; }
TextInput { width: 180px; height: 28px; background-color: #0f172a; }
)");

    tessera::runtime::ManualClock clock;
    tessera::runtime::RuntimeLoop loop(clock);
    tessera::app::HostConfig config;
    config.width = 320;
    config.height = 200;
    tessera::app::AppHost host(root, sheet, loop, config);
    host.set_update_scheduler(&scheduler);
    (void)host.begin_frame(0.0);

    Node* button = find_kind(root, "Button");
    Node* input = find_kind(root, "TextInput");
    if (!expect(button != nullptr, "generated Button exists") ||
        !expect(input != nullptr, "generated TextInput exists")) {
        return 1;
    }
    const Rect button_box = button->world_bounds();

    tessera::ui::InputPump pump;
    // Default state.
    auto color = paint_button(host, root, button_box);
    if (!expect(color.has_value() && same_color(*color, kDefault),
                "button starts at its base color")) {
        return 2;
    }

    // Pointer move over the button -> :hover pseudo-state -> repaint.
    (void)pump.queue().push(
        pointer(InputEventType::PointerMove, button_box.x + 10.0f, button_box.y + 10.0f));
    const tessera::ui::InputPumpStats hover_stats = pump.pump(root, loop);
    (void)hover_stats;
    if (!expect(tessera::ui::style::has_state(button->pseudo_state(),
                                              tessera::ui::style::PseudoState::Hover),
                "pointer move sets :hover on the button")) {
        return 30;
    }
    (void)host.begin_frame(1.0 / 60.0);
    color = paint_button(host, root, button_box);
    if (!expect(color.has_value() && same_color(*color, kHover),
                "button paints :hover while over it")) {
        return 3;
    }

    // Press -> :active paints, and the focus ring does not appear for pointer modality.
    (void)pump.queue().push(
        pointer(InputEventType::PointerDown, button_box.x + 10.0f, button_box.y + 10.0f));
    (void)pump.pump(root, loop);
    (void)host.begin_frame(1.0 / 60.0);
    color = paint_button(host, root, button_box);
    if (!expect(color.has_value() && same_color(*color, kActive),
                "button paints :active on press")) {
        return 4;
    }

    // Release -> click synthesis -> generated handler -> state.
    (void)pump.queue().push(
        pointer(InputEventType::PointerUp, button_box.x + 10.0f, button_box.y + 10.0f));
    (void)pump.pump(root, loop);
    (void)host.begin_frame(1.0 / 60.0);
    host.end_frame();
    if (!expect(demo.state_count_ == 1, "the generated Click handler incremented the state")) {
        return 5;
    }

    // Focus the TextInput by pressing it (the pump router focuses focusable targets), then type:
    // the generated onChange must copy the committed text into state and the controlled node must
    // paint it.
    const Rect input_box = input->world_bounds();
    (void)pump.queue().push(
        pointer(InputEventType::PointerDown, input_box.x + 8.0f, input_box.y + 8.0f));
    (void)pump.pump(root, loop);
    (void)host.begin_frame(1.0 / 60.0);
    if (!expect(pump.router().focus_manager().focused() == input,
                "pressing the TextInput focuses it through the pump")) {
        return 6;
    }
    (void)pump.queue().push(
        pointer(InputEventType::PointerUp, input_box.x + 8.0f, input_box.y + 8.0f));
    (void)pump.queue().push(edit_text("Ada"));
    (void)pump.pump(root, loop);
    (void)host.begin_frame(1.0 / 60.0);
    host.end_frame();
    if (!expect(demo.state_name_ == "Ada", "typed text reached the generated onChange")) {
        return 7;
    }
    if (!expect(input->paint_data().has_value() && input->paint_data()->kind == PaintKind::Text &&
                    input->paint_data()->text == "Ada",
                "the controlled TextInput paints the typed value")) {
        return 7;
    }

    std::cout << "app_input: ok\n";
    return 0;
}
