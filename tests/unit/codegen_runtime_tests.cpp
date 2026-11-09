// SPDX-License-Identifier: MIT

// End-to-end runtime proof of the first codegen slice: build the generated component and inspect
// the node metadata, children, focusability and D9 text the generated `build()` attached. No GPU.

#include "Sample.hpp"
#include "tessera/ui/scene/node.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace ui = tessera::ui;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void check_eq(std::string_view actual, std::string_view expected, std::string_view message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << "\n  expected: " << expected
                  << "\n  actual:   " << actual << "\n";
        ++failures;
    }
}

} // namespace

int main() {
    demo::gen::Sample sample;
    ui::Node root(1000);
    sample.build(root);

    check(root.children().size() == 1, "the component builds exactly one root child");
    const ui::Node& view = *root.children()[0];
    check_eq(view.element_kind(), "View", "element kind metadata");
    check_eq(view.element_id(), "root", "element id metadata");
    check_eq(view.class_name(), "box", "className metadata");
    check_eq(view.role(), "generic", "role metadata");
    check(view.automation_id() == 1, "automationId metadata");
    check(view.aria_attributes().size() == 1, "one aria pair is attached");
    if (view.aria_attributes().size() == 1) {
        check_eq(view.aria_attributes()[0].name, "aria-label", "aria name");
        check_eq(view.aria_attributes()[0].value, "root", "aria value");
    }
    check(!view.focusable(), "a View is not focusable by default");
    check(!view.paint_data().has_value(), "a View has no text payload");

    check(view.children().size() == 2, "the View carries both event-bound buttons");
    const ui::Node& button = *view.children()[0];
    check_eq(button.element_kind(), "Button", "button element kind");
    check(button.focusable(), "generated Button is focusable");
    check(button.children().size() == 1, "the Button carries the Text child");

    const ui::Node& label = *button.children()[0];
    check_eq(label.element_kind(), "Text", "text element kind");
    check(label.paint_data().has_value(), "generated Text attaches a PaintData payload");
    if (label.paint_data()) {
        check_eq(label.paint_data()->text, "Hello", "text content is attached through PaintData");
    }
    check(!label.focusable(), "a Text node is not focusable");

    check(sample.state_count_ == 0, "useState cell starts at its IR initial value");

    // Synthetic dispatch: the generated adapter runs at the Target phase and the setter updates the
    // state cell (the interactive end-to-end proof).
    check(view.children().size() == 2, "the View carries both event-bound buttons");
    ui::Event click;
    click.type = ui::EventType::PointerUp;
    click.x = 12.0f;
    click.y = 8.0f;
    click.pointer_kind = ui::PointerKind::Mouse;
    click.button = ui::PointerButton::Left;
    ui::dispatch_event_to(*view.children()[1], click);
    check(sample.state_count_ == 1, "a dispatched PointerUp runs the generated setter handler");

    // The developer region binding is dispatched too, but its generated stub leaves state alone.
    ui::dispatch_event_to(*view.children()[0], click);
    check(sample.state_count_ == 1, "the binding region runs without changing unrelated state");
    sample.sync();
    sample.sync_cleanup();

    const ui::ClickEvent payload = ui::click_event(click);
    check(payload.x == 12.0f && payload.y == 8.0f, "payload record carries the pointer position");
    check(payload.button == ui::PointerButton::Left, "payload record carries the button");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen runtime tests passed\n";
    return 0;
}
