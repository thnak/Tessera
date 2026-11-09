// SPDX-License-Identifier: MIT

// End-to-end runtime proof of the first codegen slice: build the generated component and inspect
// the node metadata, children, focusability and D9 text the generated `build()` attached. No GPU.

#include "Sample.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <cstdint>
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

// Node::insert_child is the additive scene API generated components use to place a conditional
// branch or a keyed-list item at its deterministic source position. The contract is: clamped index
// (out of range appends), parent pointer and ownership move like add_child, and the same dirty
// domains (child Style|Layout|Paint|Render, parent Layout|Paint|Render).
void test_insert_child_contract() {
    ui::Node parent(100);
    parent.add_child(std::make_unique<ui::Node>(101));
    parent.add_child(std::make_unique<ui::Node>(103));
    parent.clear_dirty(ui::Dirty::All);
    ui::Node& middle = parent.insert_child(1, std::make_unique<ui::Node>(102));
    check(parent.children().size() == 3, "insert_child adds a child");
    check(parent.children()[1].get() == &middle, "insert_child uses the requested position");
    check(parent.children()[2]->id() == 103, "the following sibling moves back");
    check(middle.parent() == &parent, "the inserted child has its parent pointer");
    check(middle.id() == 102, "the inserted child keeps its id");
    check(ui::has_flag(middle.dirty(), ui::Dirty::Style) &&
              ui::has_flag(middle.dirty(), ui::Dirty::Layout) &&
              ui::has_flag(middle.dirty(), ui::Dirty::Paint) &&
              ui::has_flag(middle.dirty(), ui::Dirty::Render),
          "an inserted child is marked Style|Layout|Paint|Render");
    check(ui::has_flag(parent.dirty(), ui::Dirty::Layout) &&
              ui::has_flag(parent.dirty(), ui::Dirty::Paint) &&
              ui::has_flag(parent.dirty(), ui::Dirty::Render),
          "inserting marks the parent Layout|Paint|Render");
    // Out-of-range indexes clamp to the end rather than being an error, so a generated update that
    // walks slot positions never has to special-case "past the last child".
    ui::Node& appended = parent.insert_child(99, std::make_unique<ui::Node>(104));
    check(parent.children().size() == 4 && parent.children().back().get() == &appended,
          "an out-of-range index appends");
    ui::Node empty(200);
    ui::Node& only = empty.insert_child(0, std::make_unique<ui::Node>(201));
    check(empty.children().size() == 1 && empty.children()[0].get() == &only,
          "inserting into an empty parent works at index 0");
}

} // namespace

int main() {
    test_insert_child_contract();
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
    check(view.inline_style().size() == 2, "inline style declarations attached");
    if (view.inline_style().size() == 2) {
        check_eq(view.inline_style()[0].name, "width", "inline style name");
        check_eq(view.inline_style()[0].value, "10px", "static inline style value");
        check_eq(view.inline_style()[1].name, "opacity", "dynamic inline style name");
        check_eq(view.inline_style()[1].value, "1", "dynamic inline style value");
    }
    check(view.aria_attributes().size() == 1, "one aria pair is attached");
    if (view.aria_attributes().size() == 1) {
        check_eq(view.aria_attributes()[0].name, "aria-label", "aria name");
        check_eq(view.aria_attributes()[0].value, "label 0", "dynamic aria value");
    }
    check(!view.focusable(), "a View is not focusable by default");
    check(!view.paint_data().has_value(), "a View has no text payload");

    check(view.children().size() == 3, "the View carries both buttons and the input");
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

    // Inline style is consumed by the style tree: empty sheet + inline width resolves to 42px,
    // clearing it returns to the initial value, and both mark Dirty::Style only.
    const tessera::ui::style::CompiledStyleSheet sheet = tessera::ui::style::compile_stylesheet("");
    tessera::ui::style::SceneStyleTree tree(view);
    const tessera::ui::style::ComputedStyle initial = tree.compute(sheet, view);
    check(initial.find("width") == nullptr || *initial.find("width") != "42px",
          "the stylesheet alone does not produce the inline width");
    view.clear_dirty(ui::Dirty::All);
    check(view.set_inline_style("width", "42px"), "an inline declaration is accepted");
    tessera::ui::style::SceneStyleTree inline_tree(view);
    const tessera::ui::style::ComputedStyle overridden = inline_tree.compute(sheet, view);
    check(overridden.find("width") != nullptr && *overridden.find("width") == "42px",
          "the style tree applies the inline declaration");
    check(ui::has_flag(view.dirty(), ui::Dirty::Style), "inline style marks Dirty::Style");
    view.clear_inline_style();
    tessera::ui::style::SceneStyleTree cleared_tree(view);
    const tessera::ui::style::ComputedStyle cleared = cleared_tree.compute(sheet, view);
    check(cleared.find("width") == nullptr || *cleared.find("width") != "42px",
          "clearing inline declarations restores the cascade");
    check(view.inline_style().empty(), "inline declarations are cleared");

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
    check(view.children().size() == 3, "the View carries both buttons and the input");
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

    // Controlled TextInput: generated source-event bindings drive the runtime, the committed
    // TextChange event runs the generated setter, and apply_state paints the value.
    check(view.children().size() == 3, "the View carries the controlled TextInput");
    ui::Node& input = *view.children()[2];
    check_eq(input.element_kind(), "TextInput", "controlled field kind");
    check(input.has_event_binding(ui::EventType::TextChange),
          "onChange is bound through the typed TextChange adapter");
    check(sample.text_input_count() == 1, "one generated editor accessor");
    check_eq(sample.text_input_editor(0).buffer().text(), "", "the editor starts empty");
    input.set_pseudo_state(ui::style::PseudoState::Focus);
    input.clear_dirty(ui::Dirty::All);
    for (const char character : std::string("Ada")) {
        ui::Event text;
        text.type = ui::EventType::Text;
        text.text_codepoint = static_cast<std::uint32_t>(character);
        ui::dispatch_event_to(input, text);
    }
    check_eq(sample.state_name_, "Ada", "the generated setter copied the committed text");
    check_eq(sample.text_input_editor(0).buffer().text(), "Ada",
             "the generated runtime owns the edit buffer");
    if (input.paint_data()) {
        check_eq(input.paint_data()->text, "Ada", "apply_state painted the controlled value");
    }
    check(input.dirty() == (ui::Dirty::Paint | ui::Dirty::Render),
          "the controlled value marks Paint|Render only");
    check(!ui::has_flag(input.dirty(), ui::Dirty::Layout), "controlled text never marks Layout");
    input.clear_dirty(ui::Dirty::All);
    sample.apply_state();
    check(input.dirty() == ui::Dirty::Clean, "an unchanged controlled value marks nothing");

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
