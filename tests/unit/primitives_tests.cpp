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
    // Handler order is source order: down, up, leave, key down, key up, then the forwarded click.
    const auto& handlers = press.component->handlers;
    check(handlers.size() == 6, "six press handlers");
    if (handlers.size() == 6) {
        check(handlers[5].actions.size() == 1 &&
                  handlers[5].actions[0].kind == ir::ActionKind::CallProp &&
                  handlers[5].actions[0].target == "prop::onPress",
              "the click is forwarded to the onPress callback prop");
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

void test_checkbox() {
    Primitive checkbox = load("Checkbox");
    if (checkbox.component == nullptr) {
        return;
    }
    const ir::Node& root = checkbox.component->root;
    check_eq(prop_text(root, "role"), "checkbox", "Checkbox is a checkbox");
    check(root.props.count("aria-checked") == 1 && root.props.count("tabIndex") == 1,
          "the state is exposed and the box is focusable");
    check(checkbox.component->handlers.size() == 1, "one click handler");
    if (checkbox.component->handlers.size() == 1) {
        const auto& actions = checkbox.component->handlers[0].actions;
        check(actions.size() == 2 && actions[1].kind == ir::ActionKind::CallProp &&
                  actions[1].target == "prop::onToggle",
              "a click flips the state, then tells the caller");
        eval::Scope scope;
        scope["state::checked"] = eval::Value::of_bool(false);
        eval::Evaluator evaluator;
        check(evaluator.run(actions, scope).has_value(), "the first click runs");
        check(*scope["state::checked"].as_bool(), "the first click checks it");
        check(evaluator.run(actions, scope).has_value(), "the second click runs");
        check(!*scope["state::checked"].as_bool(), "the second click unchecks it");
    }
    generates(checkbox, "Checkbox");
}

void test_radio() {
    Primitive radio = load("Radio");
    if (radio.component == nullptr) {
        return;
    }
    const ir::Node& root = radio.component->root;
    check_eq(prop_text(root, "role"), "radio", "Radio is a radio");
    check(root.props.count("aria-checked") == 1 && root.props.count("tabIndex") == 1,
          "the group's state is exposed and the radio is focusable");
    check(radio.component->state.empty(), "the checked state belongs to the group, not the radio");
    check(radio.component->handlers.size() == 1, "one click handler");
    generates(radio, "Radio");
}

void test_progress() {
    Primitive progress = load("Progress");
    if (progress.component == nullptr) {
        return;
    }
    const ir::Node& root = progress.component->root;
    check_eq(prop_text(root, "role"), "progressbar", "Progress is a progressbar");
    check(root.props.count("aria-valuenow") == 1 && root.props.count("aria-valuemin") == 1 &&
              root.props.count("aria-valuemax") == 1,
          "value and bounds are exposed");
    check(root.props.count("tabIndex") == 0, "progress is not focusable");
    check(progress.component->handlers.empty(), "progress has no handlers");
    generates(progress, "Progress");
}

void test_disclosure() {
    Primitive disclosure = load("Disclosure");
    if (disclosure.component == nullptr) {
        return;
    }
    const ir::Node& root = disclosure.component->root;
    check(root.children.size() == 2, "a summary and a content View");
    if (root.children.size() == 2) {
        const ir::Node& summary = root.children[0];
        check_eq(prop_text(summary, "role"), "button", "the summary is a button");
        check(summary.props.count("aria-expanded") == 1, "the open state is exposed");
        check(root.children[1].style.count("display") == 1, "closing hides the content");
    }
    check(disclosure.component->handlers.size() == 1, "one click handler");
    if (disclosure.component->handlers.size() == 1) {
        const auto& actions = disclosure.component->handlers[0].actions;
        eval::Scope scope;
        scope["state::open"] = eval::Value::of_bool(false);
        eval::Evaluator evaluator;
        check(evaluator.run(actions, scope).has_value(), "the first click runs");
        check(*scope["state::open"].as_bool(), "the first click opens it");
        check(evaluator.run(actions, scope).has_value(), "the second click runs");
        check(!*scope["state::open"].as_bool(), "the second click closes it");
    }
    generates(disclosure, "Disclosure");
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

// The scope, roving and list primitives are behavior hooks now (issue #643, ADR-064 item 7): the
// behavior itself is tested headlessly (`behavior_focus_trap`, `behavior_roving_focus`), so here
// the contract is that the hook call lowers to one `behaviors` entry on the element it targets and
// that codegen attaches the engine behavior to it. No wrapper component adds a node.
Primitive load_source(const std::string& name, const std::string& text) {
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

// The hook form of one former wrapper. `options` is the hook's option object.
Primitive hook_primitive(const std::string& name, const std::string& hook,
                         const std::string& options, const std::string& role) {
    return load_source(name,
                       "export function " + name +
                           "(): Node {\n  const box = useRef<View>(null);\n  const b = " + hook +
                           "(box, " + options + ");\n  return <View ref={box} role=\"" + role +
                           "\" aria-label=\"x\"><Text>t</Text></View>;\n}\n");
}

void expect_behavior(Primitive& primitive, const std::string& name, const std::string& behavior,
                     const std::string& needle) {
    if (primitive.component == nullptr) {
        return;
    }
    check(primitive.component->behaviors.size() == 1, name + " has one behavior entry");
    if (primitive.component->behaviors.size() == 1) {
        check_eq(primitive.component->behaviors.front().behavior, behavior,
                 name + " names the engine behavior");
    }
    const std::string source = generates(primitive, name);
    check(source.find("attach(\"" + behavior + "\"") != std::string::npos,
          name + " attaches the registry behavior");
    check(needle.empty() || source.find(needle) != std::string::npos,
          name + " passes its option: " + needle);
}

void test_focus_scope() {
    Primitive scope = hook_primitive("FocusScope", "useFocusTrap", "{}", "group");
    expect_behavior(scope, "FocusScope", "focus_trap", "");
}

void test_roving_focus() {
    Primitive row =
        hook_primitive("RovingFocus", "useListFocus", "{ orientation: \"horizontal\" }", "group");
    expect_behavior(row, "RovingFocus", "roving_focus", "horizontal");
    Primitive column = hook_primitive("RovingFocusVertical", "useListFocus",
                                      "{ orientation: \"vertical\" }", "group");
    expect_behavior(column, "RovingFocusVertical", "roving_focus", "vertical");
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
    // The key handler is the last of the five (size, down, move, up, key).
    check(evaluator.run(slider.handlers.back().actions, scope).has_value(),
          "the slider handler runs");
    return *scope["state::value"].as_int();
}

// Runs handler `index` (0 size, 1 down, 2 move, 3 up) on a 200 px track and returns the
// scope so the caller reads `state::value` and `state::dragging`.
eval::Scope drag(const ir::Component& slider, std::size_t index, double x, bool dragging,
                 double width = 200.0) {
    eval::Scope scope;
    scope["state::value"] = eval::Value::of_int(50);
    scope["state::width"] = eval::Value::of_float(width);
    scope["state::dragging"] = eval::Value::of_bool(dragging);
    scope["e.x"] = eval::Value::of_float(x);
    scope["e.width"] = eval::Value::of_float(width);
    scope["prop::min"] = eval::Value::of_int(0);
    scope["prop::max"] = eval::Value::of_int(100);
    scope["prop::step"] = eval::Value::of_int(5);
    eval::Evaluator evaluator;
    check(evaluator.run(slider.handlers[index].actions, scope).has_value(),
          "the slider pointer handler runs");
    return scope;
}

void test_slider_drag(const ir::Component& slider) {
    check(*drag(slider, 0, 0.0, false, 320.0)["state::width"].as_float() == 320.0,
          "onSizeChange stores the track width");
    auto down = drag(slider, 1, 100.0, false);
    check(*down["state::value"].as_int() == 50, "a press at mid-track picks the middle");
    check(*down["state::dragging"].as_bool(), "a press starts the drag");
    check(*drag(slider, 1, 51.0, false)["state::value"].as_int() == 25,
          "the fraction snaps to the step");
    check(*drag(slider, 2, 150.0, true)["state::value"].as_int() == 75, "a move while dragging");
    check(*drag(slider, 2, -40.0, true)["state::value"].as_int() == 0,
          "a captured move left of the box clamps to min");
    check(*drag(slider, 2, 500.0, true)["state::value"].as_int() == 100,
          "a captured move past the width clamps to max");
    check(*drag(slider, 2, 150.0, false)["state::value"].as_int() == 50,
          "a move without a press changes nothing");
    check(*drag(slider, 1, 150.0, false, 0.0)["state::value"].as_int() == 50,
          "a zero width (before the first layout) changes nothing");
    check(!*drag(slider, 3, 0.0, true)["state::dragging"].as_bool(), "release ends the drag");
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
    check(slider.component->handlers.size() == 5, "size, three pointer and one key handler");
    if (slider.component->handlers.size() == 5) {
        const ir::Component& component = *slider.component;
        test_slider_drag(component);
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
    Primitive list = hook_primitive("ListNavigation", "useListFocus",
                                    "{ orientation: \"vertical\", typeahead: true }", "listbox");
    expect_behavior(list, "ListNavigation", "roving_focus", "vertical");
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
    test_checkbox();
    test_radio();
    test_progress();
    test_disclosure();
    if (jsx_test::failures != 0) {
        std::cerr << jsx_test::failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "primitives tests passed\n";
    return 0;
}
