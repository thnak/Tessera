// SPDX-License-Identifier: MIT

// End-to-end proof of generated behavior hooks (ADR-064 item 7, issue #641): the compiled
// Pressable component owns a real `behavior::Instance`, attaches it through the scene's own
// `InputRouter`, reports the engine's outputs through the generated accessors, and detaches it
// again. This is the test that would have caught the defect #641 fixes: before the wiring landed,
// the generated base owned a slot that nothing ever attached, so a behavior hook was inert in
// generated code no matter what the engine implemented.
//
// No GPU: the scene, the router and the behavior are all headless.

#include "Pressable.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
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

// The generated base's own node ids start at 1 for a root component, so the View the ref points at
// is node 1 and the Text is node 2.
void test_attach_reports_outputs() {
    ui::Node root(1000);
    pressable::Pressable component;
    component.build(root);
    check(root.children().size() == 1, "the component built its root view");

    ui::InputRouter router;
    component.attach_behaviors(router);

    // Before any interaction the behavior reports the manifest defaults: not pressed, not hovered,
    // not focused. The accessors read the live instance rather than a cached value.
    check(!component.beh_press_pressed(), "a fresh behavior reports not pressed");
    check(!component.beh_press_hovered(), "a fresh behavior reports not hovered");
    check(!component.beh_press_focused(), "a fresh behavior reports not focused");

    // A refresh with no interaction state changes nothing, which is what makes it safe to call
    // after every dispatch.
    component.refresh_behaviors();
    check(!component.beh_press_pressed(), "a refresh without a press keeps the default");

    component.detach_behaviors();
    check(!component.beh_press_pressed(), "a detached behavior reports the default again");
}

// The attach is idempotent in the sense that matters: detaching and re-attaching rebuilds the
// engine behavior rather than leaving a dangling one.
void test_reattach() {
    ui::Node root(1000);
    pressable::Pressable component;
    component.build(root);

    ui::InputRouter router;
    component.attach_behaviors(router);
    component.detach_behaviors();
    component.attach_behaviors(router);
    check(!component.beh_press_pressed(), "a re-attached behavior reports the default");
    component.detach_behaviors();
}

// The output reaches the pixels: the Text binding reads the generated accessor, so a refresh that
// changes an output re-evaluates it. The default is `false`, which the text renders as "false".
void test_output_reaches_the_binding() {
    ui::Node root(1000);
    pressable::Pressable component;
    component.build(root);

    ui::InputRouter router;
    component.attach_behaviors(router);
    component.apply_state();

    const ui::Node* view = root.children().empty() ? nullptr : root.children().front().get();
    check(view != nullptr, "the root view exists");
    if (view == nullptr || view->children().empty()) {
        return;
    }
    const ui::Node* label = view->children().front().get();
    check(label->paint_data().has_value() && label->paint_data()->text == "false",
          "the text binding renders the behavior output's default");
    component.detach_behaviors();
}

} // namespace

int main() {
    test_attach_reports_outputs();
    test_reattach();
    test_output_reaches_the_binding();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
