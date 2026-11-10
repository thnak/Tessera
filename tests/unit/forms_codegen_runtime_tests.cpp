// SPDX-License-Identifier: MIT

// End-to-end runtime proof of the generated form-state binding (task-103): the compiled `FormPage`
// seeds a `tessera::ui::forms::Form` controller from `initial`, a text edit raised on the generated
// Field widget routes into the controller through the generated `TextChange` adapter, the Field's
// value/error repaint from the controller, submit focuses and reports the first invalid Field, and
// async validation stays on the `CompletionQueue` (submit returns `Pending`, never blocks). No GPU.
//
// The standalone `FieldPage` keeps the pre-`Form` explicit-prop coverage: a scene click drives the
// generated handler and the value prop refresh.

#include "Field.hpp"
#include "FieldPage.hpp"
#include "Form.hpp"
#include "FormPage.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/task.hpp"
#include "tessera/ui/forms/form_model.hpp"
#include "tessera/ui/scene/focus.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace ui = tessera::ui;
namespace forms = tessera::ui::forms;
namespace runtime = tessera::runtime;

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

std::string_view paint_text(const ui::Node& node) {
    return node.paint_data() ? node.paint_data()->text : std::string_view("<none>");
}

void dispatch_text(ui::Node& node, const std::string& value) {
    ui::Event event;
    event.type = ui::EventType::TextChange;
    event.text_view = value.data();
    event.text_view_size = static_cast<std::uint32_t>(value.size());
    check(ui::dispatch_event_to(node, event), "the generated TextChange binding dispatched");
}

// FormPage > View(class `form-page`) > Component host > the library Form's View(class `form`) >
// the caller's Field hosts (slot content).
ui::Node* form_view(ui::Node& root) {
    if (root.children().empty() || root.children()[0]->children().empty()) {
        return nullptr;
    }
    ui::Node& host = *root.children()[0]->children()[0];
    return host.children().empty() ? nullptr : host.children()[0].get();
}

ui::Node* field_host(ui::Node& root, std::size_t index) {
    ui::Node* view = form_view(root);
    if (view == nullptr || view->children().size() <= index) {
        return nullptr;
    }
    return view->children()[index].get();
}

ui::Node* field_part(ui::Node& root, std::size_t index, std::size_t part) {
    ui::Node* host = field_host(root, index);
    if (host == nullptr || host->children().empty() ||
        host->children()[0]->children().size() <= part) {
        return nullptr;
    }
    return host->children()[0]->children()[part].get();
}

ui::Node* field_widget(ui::Node& root, std::size_t index) {
    return field_part(root, index, 3);
}

// FieldPage > View > [Button, Field host] > the library Field View > [label, value, error, widget].
ui::Node* standalone_field_value(ui::Node& root) {
    if (root.children().empty() || root.children()[0]->children().size() < 2) {
        return nullptr;
    }
    ui::Node& host = *root.children()[0]->children()[1];
    if (host.children().empty() || host.children()[0]->children().size() < 2) {
        return nullptr;
    }
    return host.children()[0]->children()[1].get();
}

void test_the_form_builds_through_the_controller() {
    demo::gen::FormPage page;
    ui::Node root(1000);
    page.build(root);

    check(root.children().size() == 1, "the page builds one root child");
    if (root.children().empty()) {
        return;
    }
    check_eq(root.children()[0]->class_name(), "form-page", "the page root class");
    check(root.children()[0]->children().size() == 1, "the page owns the Form host node");
    if (root.children()[0]->children().empty()) {
        return;
    }
    check_eq(root.children()[0]->children()[0]->element_kind(), "Component",
             "the Form is an ordinary Component host node");
    ui::Node* view = form_view(root);
    check(view != nullptr, "the library Form rendered its container");
    if (view == nullptr) {
        return;
    }
    check_eq(view->class_name(), "form", "the library Form rendered its container");
    check(view->children().size() == 2, "both Fields ride the Form's Children slot");

    check_eq(paint_text(*field_part(root, 0, 0)), "Email", "the email label reached the Field");
    check_eq(paint_text(*field_part(root, 1, 0)), "Age", "the age label reached the Field");
    check_eq(paint_text(*field_part(root, 0, 1)), "", "the email seed starts empty");
    check_eq(paint_text(*field_part(root, 1, 1)), "0", "the age seed came from `initial`");
    check_eq(paint_text(*field_part(root, 0, 2)), "", "no error before validation");

    // The controller was seeded from `initial` and carries the generated rule records.
    check(page.form_2().size() == 2, "the controller seeded two fields");
    check(page.form_2().find_field("email").has_value(), "the email field is a stable path");
    check(page.form_2().find_field("age").has_value(), "the age field is a stable path");
    check(page.form_2().rule_count(page.form_controller_2_id_email_) == 2,
          "required + minLength became two rule records");
    check(page.form_2().rule_count(page.form_controller_2_id_age_) == 0,
          "a Field without rules adds none");
    check(!page.form_2().can_submit(), "an untouched form is not submittable");
}

void test_an_edit_routes_into_the_controller() {
    demo::gen::FormPage page;
    ui::Node root(2000);
    page.build(root);
    page.mount();
    ui::Node* widget = field_widget(root, 0);
    check(widget != nullptr, "the email widget exists");
    if (widget == nullptr) {
        return;
    }
    check_eq(widget->element_kind(), "TextInput",
             "the Field's slot root is the caller's TextInput");

    dispatch_text(*widget, "ab");

    const forms::FieldState* state = page.form_2().field("email");
    check(state != nullptr, "the controller has the edited field");
    if (state == nullptr) {
        return;
    }
    check_eq(state->value.text, "ab", "the edit reached the controller value");
    check_eq(paint_text(*field_part(root, 0, 1)), "ab",
             "the Field value repainted from the controller");
    check_eq(paint_text(*field_part(root, 0, 2)), "Too short",
             "the Field error repainted from the controller");
    check_eq(page.child_3_.prop_value_, "ab", "the generated prop record was refreshed");
    check(page.child_3_.prop_errors_.size() == 1 &&
              page.child_3_.prop_errors_.front() == "Too short",
          "the Field errors list carries the issue");

    page.unmount();
}

void test_submit_focuses_and_reports_the_first_invalid_field() {
    demo::gen::FormPage page;
    ui::Node root(3000);
    page.build(root);
    ui::FocusManager focus;
    focus.set_root(root);
    page.set_form_focus(&focus);

    ui::Node* email_widget = field_widget(root, 0);
    check(email_widget != nullptr, "the email widget exists for focus");
    if (email_widget == nullptr) {
        return;
    }

    // Submit with an untouched required field: the controller validates, the binding reports the
    // field name and focuses its widget.
    const forms::SubmitResult invalid = page.submit_form_2();
    check(invalid.status == forms::SubmitStatus::Invalid, "an untouched form is invalid");
    check(invalid.first_invalid == page.form_controller_2_id_email_,
          "the first invalid field is the email field");
    check_eq(page.form_controller_2_report_, "email", "submit reports the field by name");
    check(focus.focused() == email_widget, "submit focused the first invalid field's widget");
    check_eq(paint_text(*field_part(root, 0, 2)), "Required",
             "a submit surfaces the first invalid field's error");

    // A valid edit + touch settles the form and routes to the submitted callback.
    dispatch_text(*email_widget, "ada@example.com");
    page.form_2().touch(page.form_controller_2_id_email_);
    page.form_2().touch(page.form_controller_2_id_age_);
    bool submitted = false;
    page.child_2_.prop_onSubmit_ = [&submitted]() { submitted = true; };
    bool observed_validate = false;
    page.child_2_.prop_validate_ = [&observed_validate]() { observed_validate = true; };

    check(page.validate_form_2(), "the settled form validates");
    check(observed_validate, "validate routed to the validate callback prop");
    const forms::SubmitResult ok = page.submit_form_2();
    check(ok.status == forms::SubmitStatus::Ok, "the settled form submits");
    check(submitted, "a successful submit invoked the onSubmit callback prop");
    check(page.form_controller_2_report_.empty(), "a successful submit clears the report");
    check_eq(paint_text(*field_part(root, 0, 2)), "", "a valid field shows no error");
}

runtime::Task<bool> not_taken(runtime::TaskContext, const forms::FormValue& value) {
    co_return value.text != "taken";
}

void test_async_validation_stays_on_the_completion_queue() {
    runtime::CompletionQueue queue(8);
    demo::gen::FormPage page;
    page.form_controller_2_config_.completion_queue = &queue;
    ui::Node root(4000);
    page.build(root);

    check(page.form_2().set_async_validator(page.form_controller_2_id_email_, &not_taken),
          "an async validator attaches to the seeded controller");
    ui::Node* async_widget = field_widget(root, 0);
    check(async_widget != nullptr, "the async widget exists");
    if (async_widget == nullptr) {
        return;
    }
    dispatch_text(*async_widget, "taken");
    check(page.form_2().touch(page.form_controller_2_id_email_),
          "touch schedules async validation");
    const forms::FieldState* pending = page.form_2().field("email");
    check(pending != nullptr && pending->state == forms::ValidationState::Pending,
          "the field is Pending while the validator is in flight");
    check(queue.size() > 0, "the async bridge is queued, not run inline");

    // Submit returns immediately with Pending: no UI-thread wait for the validator.
    const forms::SubmitResult in_flight = page.submit_form_2();
    check(in_flight.status == forms::SubmitStatus::Pending,
          "submit reports Pending without blocking");
    check(in_flight.pending == 1, "the pending field count is reported");

    queue.run(8);
    page.apply_state();
    const forms::SubmitResult settled = page.submit_form_2();
    check(settled.status == forms::SubmitStatus::Invalid,
          "the settled async rejection blocks submit");
    check_eq(page.form_controller_2_report_, "email", "the rejected field is reported");
    check_eq(paint_text(*field_part(root, 0, 2)), "Invalid",
             "the async issue repainted through the controller");
}

void test_the_standalone_field_still_uses_explicit_props() {
    demo::gen::FieldPage page;
    ui::Node root(5000);
    page.build(root);
    page.mount();
    if (root.children().empty() || root.children()[0]->children().size() < 2) {
        check(false, "the standalone Field page built");
        return;
    }
    ui::Node& form = *root.children()[0];
    ui::Node* value = standalone_field_value(root);
    check(value != nullptr, "the standalone Field value text exists");
    if (value == nullptr) {
        return;
    }
    ui::Event click;
    click.type = ui::EventType::PointerUp;
    check(ui::dispatch_event_to(*form.children()[0], click), "the Button binding dispatched");
    page.update();
    check_eq(paint_text(*value), "ada@example.com",
             "the state change reached the Field through apply_props()/apply_state()");
    page.unmount();
}

void test_the_build_is_deterministic() {
    const auto snapshot = [](ui::Node& root) {
        std::string out;
        for (const auto& child : root.children()) {
            out +=
                std::string(child->element_kind()) + ":" + std::string(child->class_name()) + ";";
        }
        return out;
    };
    demo::gen::FormPage first;
    ui::Node first_root(6000);
    first.build(first_root);
    demo::gen::FormPage second;
    ui::Node second_root(6000);
    second.build(second_root);
    check_eq(snapshot(first_root), snapshot(second_root), "two builds produce the same tree");
}

} // namespace

int main() {
    test_the_form_builds_through_the_controller();
    test_an_edit_routes_into_the_controller();
    test_submit_focuses_and_reports_the_first_invalid_field();
    test_async_validation_stays_on_the_completion_queue();
    test_the_standalone_field_still_uses_explicit_props();
    test_the_build_is_deterministic();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "forms codegen runtime tests passed\n";
    return 0;
}
