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
    ui::Node& view = *root.children()[0];
    check_eq(view.element_kind(), "View", "element kind metadata");
    check_eq(view.element_id(), "root-0", "dynamic id metadata");
    check_eq(view.class_name(), "box 0", "dynamic className metadata");
    check_eq(view.role(), "generic", "dynamic role metadata");
    check(view.automation_id() == 1, "automationId metadata");
    check(view.aria_attributes().size() == 1, "one aria pair is attached");
    if (view.aria_attributes().size() == 1) {
        check_eq(view.aria_attributes()[0].name, "aria-label", "aria name");
        check_eq(view.aria_attributes()[0].value, "label 0", "dynamic aria value");
    }
    check(!view.focusable(), "a View is not focusable by default");
    check(!view.paint_data().has_value(), "a View has no text payload");

    check(view.children().size() == 2, "the View carries both event-bound buttons");
    ui::Node& button = *view.children()[0];
    check_eq(button.element_kind(), "Button", "button element kind");
    check(button.focusable(), "generated Button is focusable");
    check(button.children().size() == 1, "the Button carries the Text child");

    ui::Node& label = *button.children()[0];
    check_eq(label.element_kind(), "Text", "text element kind");
    check(label.paint_data().has_value(), "generated Text attaches a PaintData payload");
    if (label.paint_data()) {
        check_eq(label.paint_data()->text, "Count: 0",
                 "interpolated text is attached through PaintData");
    }
    check(!label.focusable(), "a Text node is not focusable");

    check(sample.state_count_ == 0, "useState cell starts at its IR initial value");
    check(sample.ref_box_ != nullptr, "useRef is assigned its target node");
    if (sample.ref_box_ != nullptr) {
        check_eq(sample.ref_box_->element_kind(), "Button", "ref target is the Button node");
        check(sample.ref_box_->id() == 2, "ref target has the IR node id");
    }

    // Effects: mount runs once, an unchanged deps signature does not rerun.
    sample.mount();
    check(sample.effect_sync_state_.runs == 1 && sample.effect_sync_state_.active,
          "mount runs the effect once");
    sample.update_effects();
    check(sample.effect_sync_state_.runs == 1, "unchanged deps do not rerun the effect");

    // Synthetic dispatch: the generated adapter runs at the Target phase and the setter updates the
    // state cell (the interactive end-to-end proof).
    check(view.children().size() == 2, "the View carries both event-bound buttons");
    ui::Event click;
    click.type = ui::EventType::PointerUp;
    click.x = 12.0f;
    click.y = 8.0f;
    click.pointer_kind = ui::PointerKind::Mouse;
    click.button = ui::PointerButton::Left;
    view.clear_dirty(ui::Dirty::All);
    label.clear_dirty(ui::Dirty::All);
    ui::dispatch_event_to(*view.children()[1], click);
    check(sample.state_count_ == 1, "a dispatched PointerUp runs the generated setter handler");
    check(label.dirty() == (ui::Dirty::Paint | ui::Dirty::Render),
          "the setter applies interpolation and marks Paint|Render only");
    check_eq(view.element_id(), "root-1", "the setter refreshed the dynamic id");
    check_eq(view.class_name(), "box 1", "the setter refreshed the dynamic className");
    check_eq(view.role(), "status", "the setter refreshed the dynamic role");
    check(ui::has_flag(view.dirty(), ui::Dirty::Style),
          "id/className updates mark Dirty::Style through the node setters");
    check(!ui::has_flag(view.dirty(), ui::Dirty::Layout),
          "metadata interpolation never invalidates layout");
    if (view.aria_attributes().size() == 1) {
        check_eq(view.aria_attributes()[0].value, "label 1", "the setter refreshed the aria value");
    }
    check(!ui::has_flag(label.dirty(), ui::Dirty::Layout), "interpolation is paint-only");
    if (label.paint_data()) {
        check_eq(label.paint_data()->text, "Count: 1",
                 "the setter refreshed the interpolated text");
    }

    // update() runs the changed effect; the text hash is unchanged, so it marks no new dirty bits.
    sample.update();
    check(sample.effect_sync_state_.runs == 2,
          "update() runs the effect for the changed dependency");
    label.clear_dirty(ui::Dirty::All);
    sample.update();
    check(label.dirty() == ui::Dirty::Clean, "an unchanged update marks no dirty domain");
    check(sample.effect_sync_state_.runs == 2, "the new deps signature is stable");

    // The developer region binding is dispatched too, but its stub leaves state alone.
    ui::dispatch_event_to(*view.children()[0], click);
    check(sample.state_count_ == 1, "the binding region runs without changing unrelated state");

    sample.unmount();
    check(!sample.effect_sync_state_.active, "unmount runs the cleanup and deactivates the effect");
    check(sample.effect_sync_state_.runs == 2, "unmount does not rerun the effect");

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
