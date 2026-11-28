// SPDX-License-Identifier: MIT
//
// The IME anchor (issue #1053, follow-up of #55): the focused text field's caret rect reaches
// `hal::Window::set_text_input_rect`, in window pixels, only when it changes, and tracks the caret.
// Drives the app's plain function over the fake window, so no Vulkan WindowHost is needed.

#include "fake_hal.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "window_text_input_rect.hpp"

#include <iostream>
#include <string>

namespace {

using namespace tessera;
using harness::testing::FakeWindow;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "text_input_rect: " << message << "\n";
        ++failures;
    }
    return condition;
}

class FixedStyles final : public ui::paint::StyleProvider {
public:
    [[nodiscard]] ui::style::ComputedStyle compute(const ui::Node&) override {
        return {};
    }
};

void set_caret(ui::Node& field, std::string text, std::size_t caret) {
    ui::PaintData data;
    data.kind = ui::PaintKind::Text;
    data.text = std::move(text);
    data.text_style.font_size = 16.0F;
    ui::TextDecoration decoration;
    decoration.caret_byte_offset = caret;
    data.decoration = decoration;
    field.set_paint_data(std::move(data));
}

} // namespace

int main() {
    FakeWindow window;
    FixedStyles styles;
    ui::Node root(1);
    root.set_bounds({0, 0, 400, 300});
    ui::Node& field = root.add_child(std::make_unique<ui::Node>(2));
    field.set_element_kind("TextInput");
    field.set_bounds({50, 40, 200, 30});
    app::detail::TextInputRectState state;
    using app::detail::sync_text_input_rect;

    // Nothing focused, or a focused node with no caret decoration: no anchor.
    expect(!sync_text_input_rect(window, nullptr, styles, nullptr, 1.0F, state),
           "no focus, no call");
    expect(!sync_text_input_rect(window, &field, styles, nullptr, 1.0F, state),
           "a node without a caret decoration publishes nothing");
    expect(window.text_input_rect_requests() == 0, "no request reached the window");

    // A focused field publishes its caret rect inside its own bounds.
    set_caret(field, "hello", 0);
    expect(sync_text_input_rect(window, &field, styles, nullptr, 1.0F, state), "focus publishes");
    const hal::TextInputRect first = window.last_text_input_rect();
    expect(first.x >= 50.0F && first.x <= 250.0F, "caret x lies inside the field");
    expect(first.y >= 40.0F && first.y <= 70.0F, "caret y lies inside the field");
    expect(first.height > 0.0F, "caret has a height");

    // An unchanged caret makes no second call.
    expect(!sync_text_input_rect(window, &field, styles, nullptr, 1.0F, state), "static is quiet");
    expect(window.text_input_rect_requests() == 1, "one request so far");

    // Moving the caret right moves the anchor right on the same line.
    set_caret(field, "hello", 5);
    expect(sync_text_input_rect(window, &field, styles, nullptr, 1.0F, state),
           "caret move publishes");
    expect(window.last_text_input_rect().x > first.x, "caret at the end is right of the start");
    expect(window.last_text_input_rect().y == first.y, "same line, same y");

    // Moving the field moves the anchor with it.
    const float before = window.last_text_input_rect().x;
    field.set_bounds({150, 40, 200, 30});
    expect(sync_text_input_rect(window, &field, styles, nullptr, 1.0F, state),
           "bounds move publishes");
    expect(window.last_text_input_rect().x == before + 100.0F, "anchor follows the field");

    // While composing, the anchor is the preedit start, not the caret after the preedit.
    {
        ui::PaintData data = *field.paint_data();
        data.decoration->composition_active = true;
        data.decoration->preedit_begin = 0;
        data.decoration->preedit_end = 0;
        data.decoration->preedit_text = "xin";
        field.set_paint_data(std::move(data));
        const std::optional<hal::TextInputRect> composing =
            app::detail::focused_caret_rect(&field, styles, nullptr, 1.0F);
        expect(composing && composing->x == first.x + 100.0F,
               "composition anchors at the preedit start");
    }

    if (failures == 0) {
        std::cout << "text_input_rect: ok\n";
    }
    return failures;
}
