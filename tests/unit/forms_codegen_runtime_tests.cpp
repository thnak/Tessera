// SPDX-License-Identifier: MIT

// End-to-end runtime proof of `Field` on the ordinary component path (task-90): the generated form
// builds two Field instances, each Field is an ordinary component with its own tree and slot, the
// caller's `value` prop flows into the instance, and a state change drives the generated
// `apply_props()`/`apply_state()` chain to repaint the field text. No GPU, no validation logic in
// generated code (the library owns the rules; see `forms_codegen_tests`).

#include "Field.hpp"
#include "FormPage.hpp"
#include "tessera/ui/scene/node.hpp"

#include <iostream>
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

void clear_dirty(ui::Node& node) {
    node.clear_dirty(ui::Dirty::All);
    for (const auto& child : node.children()) {
        clear_dirty(*child);
    }
}

std::string_view paint_text(const ui::Node& node) {
    return node.paint_data() ? node.paint_data()->text : std::string_view("<none>");
}

// The generated library tree is View(class `field`) > Text(label) | Text(value) | Text(error) |
// the caller's slot widget.
const ui::Node* field_root(const ui::Node& host) {
    const auto& children = host.children();
    return children.empty() ? nullptr : children[0].get();
}

const ui::Node* field_text(const ui::Node& host, std::size_t index) {
    const ui::Node* root = field_root(host);
    if (root == nullptr || root->children().size() <= index) {
        return nullptr;
    }
    return root->children()[index].get();
}

void test_the_form_builds_two_fields() {
    demo::gen::FormPage page;
    ui::Node root(1000);
    page.build(root);

    check(root.children().size() == 1, "the form builds one root child");
    if (root.children().empty()) {
        return;
    }
    ui::Node& form = *root.children()[0];
    check_eq(form.element_kind(), "View", "the form root is a View");
    check_eq(form.class_name(), "form", "the form root class");
    check(form.children().size() == 3, "two Fields and the state-changing Button");

    const ui::Node& button = *form.children()[0];
    check_eq(button.element_kind(), "Button", "the first child is the caller's Button");

    const ui::Node& email = *form.children()[1];
    check_eq(email.element_kind(), "Component", "the email Field is a Component host node");
    const ui::Node& age = *form.children()[2];
    check_eq(age.element_kind(), "Component", "the age Field is a Component host node");

    // Each instance attaches its own library tree and the caller's widget at its Slot.
    const ui::Node* email_root = field_root(email);
    const ui::Node* age_root = field_root(age);
    check(email_root != nullptr && age_root != nullptr, "both Fields attach their library tree");
    if (email_root == nullptr || age_root == nullptr) {
        return;
    }
    check_eq(email_root->class_name(), "field", "the library Field root class");
    check(email_root->children().size() == 4, "label, value, error and the caller's TextInput");
    const ui::Node& widget = *email_root->children()[3];
    check_eq(widget.element_kind(), "TextInput", "the caller's widget rides the Children slot");

    check_eq(paint_text(*field_text(email, 0)), "Email", "the label prop reached the field");
    check_eq(paint_text(*field_text(email, 1)), "", "the controlled value starts empty");
    check_eq(paint_text(*field_text(email, 2)), "", "the error prop starts empty");
    check_eq(paint_text(*field_text(age, 0)), "Age", "the second field has its own label");
    check_eq(paint_text(*field_text(age, 1)), "0", "the second field has its own value");

    // The caller's props landed on the generated instance members: the library data is passed
    // through, and only the first field got a callback binding.
    check(page.child_3_.prop_validators_.size() == 2 &&
              page.child_3_.prop_validators_[0] == "required" &&
              page.child_3_.prop_validators_[1] == "minLength:3",
          "the validators prop record reached the instance");
    check(page.child_3_.prop_required_, "the required prop record reached the instance");
    check(static_cast<bool>(page.child_3_.prop_onValidate_),
          "the onValidate callback prop bound a generated callable");
    check(!static_cast<bool>(page.child_5_.prop_onValidate_),
          "a Field without onValidate keeps an empty callback");
}

void test_a_value_change_repaints_through_the_generated_paths() {
    demo::gen::FormPage page;
    ui::Node root(2000);
    page.build(root);
    page.mount();
    if (root.children().empty() || root.children()[0]->children().size() < 3) {
        check(false, "the form tree exists");
        return;
    }
    ui::Node& form = *root.children()[0];
    ui::Node& email = *form.children()[1];
    const ui::Node* email_value = field_text(email, 1);
    check(email_value != nullptr, "the email value text exists");
    if (email_value == nullptr) {
        return;
    }
    clear_dirty(root);

    // A scene event on the caller's Button runs the generated handler and the host update path.
    ui::Event click;
    click.type = ui::EventType::PointerUp;
    check(ui::dispatch_event_to(*form.children()[0], click), "the Button binding dispatched");
    page.update();

    check_eq(paint_text(*field_text(email, 1)), "ada@example.com",
             "the state change reached the Field value through apply_props()/apply_state()");
    const ui::Node* age_value = field_text(*form.children()[2], 1);
    check(age_value != nullptr, "the age value text exists");
    if (age_value != nullptr) {
        check_eq(paint_text(*age_value), "0", "the static second field is unchanged");
    }
    check(ui::has_flag(email_value->dirty(), ui::Dirty::Paint) &&
              ui::has_flag(email_value->dirty(), ui::Dirty::Render),
          "the repaint marks Paint|Render");
    check(!ui::has_flag(email_value->dirty(), ui::Dirty::Layout),
          "a same-box text change never marks Layout");

    page.unmount();
}

void test_the_build_is_deterministic() {
    auto texts = [](ui::Node& root) {
        std::string out;
        for (const auto& child : root.children()) {
            out += std::string(child->element_kind()) + ":";
            if (child->paint_data()) {
                out += std::string(child->paint_data()->text) + ";";
            }
        }
        return out;
    };
    demo::gen::FormPage first;
    ui::Node first_root(3000);
    first.build(first_root);
    demo::gen::FormPage second;
    ui::Node second_root(3000);
    second.build(second_root);
    check_eq(texts(first_root), texts(second_root), "two builds produce the same tree and text");
}

} // namespace

int main() {
    test_the_form_builds_two_fields();
    test_a_value_change_repaints_through_the_generated_paths();
    test_the_build_is_deterministic();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "forms codegen runtime tests passed\n";
    return 0;
}
