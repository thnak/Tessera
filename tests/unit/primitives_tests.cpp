// SPDX-License-Identifier: MIT

// Behavior primitives of packages/tessera-ui/primitives (issue #236): each file is compiled through
// the ordinary parser + analyzer + lowering + codegen path, its accessibility contract is read from
// the IR, and its handlers run headless on the IR evaluator.

#include "lowering_module_test_util.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/eval/evaluator.hpp"

#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef TESSERA_PRIMITIVES_DIR
#error "TESSERA_PRIMITIVES_DIR must be defined by the build"
#endif

namespace {

using namespace lowering_module_test;
namespace codegen = tessera::compiler::codegen;
namespace eval = tessera::compiler::eval;

struct Primitive {
    std::unique_ptr<FileFixture> keep;
    lowering::ModuleLoweringResult result;
    const ir::Component* component = nullptr;
};

Primitive load(const std::string& name) {
    std::ifstream in(std::string(TESSERA_PRIMITIVES_DIR) + "/" + name + ".tsx", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check(!text.empty(), name + ".tsx is readable");
    Primitive primitive;
    primitive.keep = parse_file("ui/" + name + ".tsx", text);
    primitive.result = lowering::lower_module(primitive.keep->module_source(), "primitives");
    check(primitive.result.has_value(), name + " lowers: " + first_message(primitive.result));
    if (primitive.result) {
        primitive.component = find_component(*primitive.result, name);
        check(primitive.component != nullptr, name + " component exists");
    }
    return primitive;
}

std::string prop_text(const ir::Node& node, const char* name) {
    const auto found = node.props.find(name);
    if (found == node.props.end() || !found->second) {
        return "<absent>";
    }
    if (found->second->kind == ir::ExprKind::Literal) {
        if (const auto* text = std::get_if<std::string>(&found->second->literal)) {
            return *text;
        }
        return "<literal>";
    }
    return "<expr>";
}

std::string generates(Primitive& primitive, const std::string& name) {
    ir::IrModule module;
    module.name = primitive.result->name;
    module.components = std::move(primitive.result->components);
    module.bindings = std::move(primitive.result->bindings);
    module.stylesheets = std::move(primitive.result->stylesheets);
    const auto generated = codegen::generate(module);
    check(generated.has_value(),
          name + " generates C++: " +
              (generated || generated.error().empty() ? std::string() : generated.error().front()));
    std::string source;
    if (generated) {
        for (const codegen::GeneratedFile& file : *generated) {
            source += file.text;
        }
    }
    return source;
}

// Runs `actions` on a scope with `state::pressed` and returns the state afterwards.
bool run_pressed(const std::vector<ir::Action>& actions, bool start, const char* key = nullptr) {
    eval::Scope scope;
    scope["state::pressed"] = eval::Value::of_bool(start);
    if (key != nullptr) {
        scope["e.key"] = eval::Value::of_string(key);
    }
    eval::Evaluator evaluator;
    check(evaluator.run(actions, scope).has_value(), "the press handler runs");
    return *scope["state::pressed"].as_bool();
}

void test_press() {
    Primitive press = load("Press");
    if (press.component == nullptr) {
        return;
    }
    const ir::Node& root = press.component->root;
    check_eq(prop_text(root, "role"), "button", "Press is a button");
    check_eq(prop_text(root, "aria-label"), "<expr>", "the name is the label prop");
    check(root.props.count("tabIndex") == 1, "Press is focusable");
    check(root.children.size() == 1 && root.children[0].kind == ir::NodeKind::Slot,
          "the content is the Children slot");
    // Handler order is source order: down, up, leave, key down, key up.
    const auto& handlers = press.component->handlers;
    check(handlers.size() == 5, "five press handlers");
    if (handlers.size() == 5) {
        check(run_pressed(handlers[0].actions, false), "pointer down presses");
        check(!run_pressed(handlers[1].actions, true), "pointer up releases");
        check(!run_pressed(handlers[2].actions, true), "pointer leave releases");
        check(run_pressed(handlers[3].actions, false, "Enter"), "Enter presses");
        check(run_pressed(handlers[3].actions, false, " "), "Space presses");
        check(!run_pressed(handlers[3].actions, false, "a"), "another key does not press");
        check(!run_pressed(handlers[4].actions, true), "key up releases");
    }
    generates(press, "Press");
}

void test_toggle() {
    Primitive toggle = load("Toggle");
    if (toggle.component == nullptr) {
        return;
    }
    const ir::Node& root = toggle.component->root;
    check_eq(prop_text(root, "role"), "switch", "Toggle is a switch");
    check(root.props.count("aria-checked") == 1, "the checked state is exposed");
    check(toggle.component->handlers.size() == 1, "one click handler");
    if (toggle.component->handlers.size() == 1) {
        // Headless: two activations flip the state off -> on -> off.
        const auto& actions = toggle.component->handlers[0].actions;
        eval::Scope scope;
        scope["state::checked"] = eval::Value::of_bool(false);
        eval::Evaluator evaluator;
        check(evaluator.run(actions, scope).has_value(), "the first click runs");
        check(*scope["state::checked"].as_bool(), "the first click turns it on");
        check(evaluator.run(actions, scope).has_value(), "the second click runs");
        check(!*scope["state::checked"].as_bool(), "the second click turns it off");
    }
    generates(toggle, "Toggle");
}

void test_dismissable_layer() {
    Primitive layer = load("DismissableLayer");
    if (layer.component == nullptr) {
        return;
    }
    const ir::Node& root = layer.component->root;
    check_eq(prop_text(root, "role"), "dialog", "the layer is a dialog");
    check(root.style.count("display") == 1, "closing hides the layer");
    check(layer.component->handlers.size() == 1, "one key handler");
    if (layer.component->handlers.size() == 1) {
        const auto& actions = layer.component->handlers[0].actions;
        for (const auto& [key, stays_open] : {std::pair<const char*, bool>{"Enter", true},
                                              std::pair<const char*, bool>{"Escape", false}}) {
            eval::Scope scope;
            scope["state::open"] = eval::Value::of_bool(true);
            scope["e.key"] = eval::Value::of_string(key);
            eval::Evaluator evaluator;
            check(evaluator.run(actions, scope).has_value(), std::string(key) + " runs");
            check(*scope["state::open"].as_bool() == stays_open,
                  std::string(key) + (stays_open ? " keeps the layer open" : " dismisses it"));
        }
    }
    generates(layer, "DismissableLayer");
}

// The scope and roving primitives carry runtime flags; the behavior itself is the headless
// `focus_scope` test (src/ui/scene/focus_scope.cpp), so here the contract is that the flag reaches
// the generated node.
void test_focus_scope() {
    Primitive scope = load("FocusScope");
    if (scope.component == nullptr) {
        return;
    }
    check_eq(prop_text(scope.component->root, "role"), "group", "the scope is a group");
    check(scope.component->root.props.count("focusTrap") == 1, "the scope traps focus");
    const std::string source = generates(scope, "FocusScope");
    check(source.find("->set_focus_trap(true);") != std::string::npos, "codegen sets the trap");
}

void test_roving_focus() {
    for (const auto& [name, axis] :
         {std::pair<const char*, const char*>{"RovingFocus", "Horizontal"},
          std::pair<const char*, const char*>{"RovingFocusVertical", "Vertical"}}) {
        Primitive group = load(name);
        if (group.component == nullptr) {
            continue;
        }
        check(group.component->root.props.count("focusGroup") == 1,
              std::string(name) + " is a focus group");
        const std::string source = generates(group, name);
        check(source.find(std::string("Node::FocusGroup::") + axis) != std::string::npos,
              std::string(name) + " sets the " + axis + " axis");
    }
}

// Runs the slider key handler and returns `state::value`.
std::int64_t slide(const ir::Component& slider, std::int64_t start, const char* key) {
    eval::Scope scope;
    scope["state::value"] = eval::Value::of_int(static_cast<std::int32_t>(start));
    scope["e.key"] = eval::Value::of_string(key);
    scope["prop::min"] = eval::Value::of_int(0);
    scope["prop::max"] = eval::Value::of_int(100);
    scope["prop::step"] = eval::Value::of_int(5);
    eval::Evaluator evaluator;
    for (const ir::Handler& handler : slider.handlers) {
        check(evaluator.run(handler.actions, scope).has_value(), "the slider handler runs");
    }
    return *scope["state::value"].as_int();
}

void test_slider() {
    Primitive slider = load("Slider");
    if (slider.component == nullptr) {
        return;
    }
    const ir::Node& root = slider.component->root;
    check_eq(prop_text(root, "role"), "slider", "the slider role");
    for (const char* name : {"aria-valuenow", "aria-valuemin", "aria-valuemax"}) {
        check(root.props.count(name) == 1, std::string(name) + " is exposed");
    }
    check(slider.component->handlers.size() == 1, "one key handler");
    if (slider.component->handlers.size() == 1) {
        const ir::Component& component = *slider.component;
        check(slide(component, 50, "ArrowRight") == 55, "ArrowRight adds a step");
        check(slide(component, 50, "ArrowUp") == 55, "ArrowUp adds a step");
        check(slide(component, 50, "ArrowLeft") == 45, "ArrowLeft subtracts a step");
        check(slide(component, 50, "PageUp") == 100, "PageUp adds ten steps, clamped");
        check(slide(component, 50, "PageDown") == 0, "PageDown subtracts ten steps, clamped");
        check(slide(component, 98, "ArrowRight") == 100, "the value clamps at max");
        check(slide(component, 2, "ArrowLeft") == 0, "the value clamps at min");
        check(slide(component, 50, "Home") == 0, "Home jumps to min");
        check(slide(component, 50, "End") == 100, "End jumps to max");
        check(slide(component, 50, "a") == 50, "another key changes nothing");
    }
    generates(slider, "Slider");
}

void test_text_field_state() {
    Primitive field = load("TextFieldState");
    if (field.component == nullptr) {
        return;
    }
    // Handlers are found by what they set: focus sets `focused`; blur clears it and sets `touched`.
    const ir::Handler* focus = nullptr;
    const ir::Handler* blur = nullptr;
    for (const ir::Handler& handler : field.component->handlers) {
        if (handler.actions.size() == 1 && handler.actions[0].target == "state::focused") {
            focus = &handler;
        } else if (handler.actions.size() == 2) {
            blur = &handler;
        }
    }
    check(focus != nullptr && blur != nullptr, "a focus and a blur handler");
    if (focus != nullptr && blur != nullptr) {
        eval::Scope scope;
        scope["state::focused"] = eval::Value::of_bool(false);
        scope["state::touched"] = eval::Value::of_bool(false);
        eval::Evaluator evaluator;
        check(evaluator.run(focus->actions, scope).has_value(), "focus runs");
        check(*scope["state::focused"].as_bool(), "focus sets focused");
        check(!*scope["state::touched"].as_bool(), "focus does not touch the field");
        check(evaluator.run(blur->actions, scope).has_value(), "blur runs");
        check(!*scope["state::focused"].as_bool(), "blur clears focused");
        check(*scope["state::touched"].as_bool(), "blur sets touched");
    }
    generates(field, "TextFieldState");
}

void test_list_navigation() {
    Primitive list = load("ListNavigation");
    if (list.component == nullptr) {
        return;
    }
    check_eq(prop_text(list.component->root, "role"), "listbox", "the list is a listbox");
    const std::string source = generates(list, "ListNavigation");
    check(source.find("Node::FocusGroup::Vertical") != std::string::npos, "vertical axis");
    check(source.find("->set_typeahead(true);") != std::string::npos, "typeahead is on");
}

} // namespace

int main() {
    test_slider();
    test_text_field_state();
    test_list_navigation();
    test_focus_scope();
    test_roving_focus();
    test_press();
    test_toggle();
    test_dismissable_layer();
    if (jsx_test::failures != 0) {
        std::cerr << jsx_test::failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "primitives tests passed\n";
    return 0;
}
