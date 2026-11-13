// SPDX-License-Identifier: MIT

// Focusable `View` and `Text` (issue #226): `tabIndex`, `autoFocus`, `onKeyDown`/`onKeyUp`/
// `onFocus`/`onBlur` (docs/specs/jsx-intrinsics-v1/common-and-events.md). Three layers:
//
// - lowering: the props and events become IR, a bad `tabIndex`/`autoFocus` and a focusable node
// with
//   no role or no accessible name are diagnostics;
// - codegen: the generated builder marks the node focusable, sets its tab index and auto-focus
//   request, and binds the key/focus events;
// - runtime, headless: a node built exactly as that code builds it is reached by Tab, receives
//   `onKeyDown`, honours `autoFocus`, and `-1` is skipped by Tab yet focusable by request.

#include "lowering_module_test_util.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/ui/key_code.hpp"
#include "tessera/ui/scene/focus.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace lowering_module_test;
namespace codegen = tessera::compiler::codegen;

constexpr std::string_view kImports = "import { KeyEvent, FocusEvent } from \"tessera\";\n";

// One component with a focusable View, wrapped so each case varies only the opening tag.
std::string page(std::string_view opening, std::string_view inside = "<Text>Toggle</Text>",
                 std::string_view closing = "</View>") {
    return std::string(kImports) +
           "function Toggle() {\n"
           "  // Note: bind::on_key\n"
           "  const onKey = (e: KeyEvent): void => {};\n"
           "  // Note: bind::on_focus\n"
           "  const onFocus = (e: FocusEvent): void => {};\n"
           "  return (\n    " +
           std::string(opening) + "\n      " + std::string(inside) + "\n    " +
           std::string(closing) + "\n  );\n}\n";
}

lowering::ModuleLoweringResult lower_page(const std::string& text,
                                          std::unique_ptr<FileFixture>& keep) {
    keep = parse_file("ui/Toggle.tsx", text);
    return lowering::lower_module(keep->module_source(), "demo");
}

const ir::Node* root_of(const lowering::LoweredModule& module) {
    const ir::Component* component = find_component(module, "Toggle");
    return component != nullptr ? &component->root : nullptr;
}

std::int64_t int_prop(const ir::Node& node, const char* name, std::int64_t missing) {
    const auto found = node.props.find(name);
    if (found == node.props.end() || !found->second ||
        found->second->kind != ir::ExprKind::Literal) {
        return missing;
    }
    const auto* value = std::get_if<std::int64_t>(&found->second->literal);
    return value != nullptr ? *value : missing;
}

constexpr std::string_view kFocusableTag =
    "<View role=\"button\" tabIndex={0} autoFocus aria-label=\"Toggle\" onKeyDown={onKey} "
    "onKeyUp={onKey} onFocus={onFocus} onBlur={onFocus}>";

void test_props_and_events_lower_to_ir() {
    std::unique_ptr<FileFixture> keep;
    const auto result = lower_page(page(kFocusableTag), keep);
    check(result.has_value(), "a focusable View lowers: " + first_message(result));
    if (!result) {
        return;
    }
    const ir::Node* root = root_of(*result);
    check(root != nullptr, "the component has a root");
    if (root == nullptr) {
        return;
    }
    check_eq(std::to_string(int_prop(*root, "tabIndex", 99)), "0", "tabIndex={0} is the literal 0");
    const auto auto_focus = root->props.find("autoFocus");
    check(auto_focus != root->props.end(), "the autoFocus flag is recorded");
    for (const char* event : {"onKeyDown", "onKeyUp", "onFocus", "onBlur"}) {
        check(root->events.find(event) != root->events.end(), std::string(event) + " is bound");
    }
}

void test_minus_one_and_text_lower() {
    std::unique_ptr<FileFixture> keep;
    const auto minus_one =
        lower_page(page("<View role=\"button\" tabIndex={-1} aria-label=\"Toggle\">"), keep);
    check(minus_one.has_value(), "tabIndex={-1} lowers: " + first_message(minus_one));
    if (minus_one && root_of(*minus_one) != nullptr) {
        check_eq(std::to_string(int_prop(*root_of(*minus_one), "tabIndex", 99)), "-1",
                 "tabIndex={-1} is the literal -1");
    }
    std::unique_ptr<FileFixture> keep_text;
    const auto text = lower_page(
        page("<Text role=\"link\" tabIndex={0} onKeyDown={onKey}>", "Docs", "</Text>"), keep_text);
    check(text.has_value(), "a focusable Text lowers: " + first_message(text));
}

void test_bad_values_are_rejected() {
    const auto rejects = [](std::string_view opening, std::string_view code, const char* why) {
        std::unique_ptr<FileFixture> keep;
        const auto result = lower_page(page(opening), keep);
        check(!result.has_value(), why);
        check_eq(first_code(result), std::string(code), why);
    };
    rejects("<View role=\"button\" tabIndex={1} aria-label=\"x\">", "TESSERA_IR_UNSUPPORTED",
            "a positive tabIndex has no V1 spelling");
    rejects("<View role=\"button\" tabIndex=\"0\" aria-label=\"x\">", "TESSERA_IR_UNSUPPORTED",
            "tabIndex must be an expression literal, not a string");
    rejects("<View role=\"button\" autoFocus aria-label=\"x\">", "TESSERA_IR_UNSUPPORTED",
            "autoFocus needs tabIndex");
}

void test_focusable_needs_a_role_and_a_name() {
    std::unique_ptr<FileFixture> keep;
    const auto no_role = lower_page(page("<View tabIndex={0} aria-label=\"Toggle\">"), keep);
    check(!no_role.has_value(), "a focusable View without a role is rejected");
    check_eq(first_code(no_role), "TESSERA_JSX_FOCUSABLE_NO_ROLE", "the missing-role code");

    std::unique_ptr<FileFixture> keep_name;
    const auto no_name =
        lower_page(page("<View role=\"button\" tabIndex={0}>", "<View />"), keep_name);
    check(!no_name.has_value(), "a focusable View without a name is rejected");
    check_eq(first_code(no_name), "TESSERA_JSX_FOCUSABLE_NO_NAME", "the missing-name code");

    std::unique_ptr<FileFixture> keep_text;
    const auto named_by_text =
        lower_page(page("<View role=\"button\" tabIndex={0}>", "<Text>Toggle</Text>"), keep_text);
    check(named_by_text.has_value(),
          "text content names the node: " + first_message(named_by_text));

    std::unique_ptr<FileFixture> keep_plain;
    const auto plain = lower_page(page("<View>", "<Text>Hi</Text>"), keep_plain);
    check(plain.has_value(), "a View without tabIndex needs no role: " + first_message(plain));
}

void test_codegen_marks_the_node_and_binds_events() {
    std::unique_ptr<FileFixture> keep;
    auto lowered = lower_page(page(kFocusableTag), keep);
    check(lowered.has_value(), "the codegen fixture lowers");
    if (!lowered) {
        return;
    }
    ir::IrModule module;
    module.name = lowered->name;
    module.components = std::move(lowered->components);
    module.bindings = std::move(lowered->bindings);
    module.stylesheets = std::move(lowered->stylesheets);
    const auto generated = codegen::generate(module);
    check(generated.has_value(), "codegen accepts a focusable View");
    if (!generated) {
        return;
    }
    std::string source;
    for (const codegen::GeneratedFile& file : *generated) {
        source += file.text;
    }
    for (const char* line :
         {"->set_focusable(true);", "->set_tab_index(0);", "->set_auto_focus(true);",
          "EventType::KeyDown", "EventType::KeyUp", "EventType::Focus", "EventType::Blur"}) {
        check(source.find(line) != std::string::npos,
              std::string("the generated code contains ") + line);
    }
}

// --- `ref.focus()` / `ref.blur()` -------------------------------------------------------------

std::string ref_page(std::string_view body) {
    return "import { useRef, useState } from \"tessera\";\n"
           "function Toggle() {\n"
           "  const target = useRef<View>(null);\n"
           "  const [count, setCount] = useState<int>(0);\n"
           "  const jump = () => { " +
           std::string(body) +
           " };\n"
           "  return (\n"
           "    <View>\n"
           "      <View ref={target} role=\"button\" tabIndex={-1} aria-label=\"Target\" />\n"
           "      <View role=\"button\" tabIndex={0} aria-label=\"Jump\" onClick={jump} />\n"
           "    </View>\n"
           "  );\n"
           "}\n";
}

void test_ref_focus_and_blur_lower_to_actions() {
    std::unique_ptr<FileFixture> keep;
    const auto result = lower_page(ref_page("target.focus(); target.blur();"), keep);
    check(result.has_value(), "ref.focus() and ref.blur() lower: " + first_message(result));
    if (!result) {
        return;
    }
    const ir::Component* component = find_component(*result, "Toggle");
    check(component != nullptr && component->handlers.size() == 1, "one inline handler");
    if (component == nullptr || component->handlers.size() != 1) {
        return;
    }
    const auto& actions = component->handlers[0].actions;
    check(actions.size() == 2, "two actions");
    if (actions.size() != 2) {
        return;
    }
    check(actions[0].kind == ir::ActionKind::Focus, "focus() is a Focus action");
    check(actions[1].kind == ir::ActionKind::Blur, "blur() is a Blur action");
    check_eq(actions[0].target, "ref::target", "the target is the ref id");
    check(actions[0].value == nullptr, "a focus action carries no value");
}

// A name that is not a ref never reaches lowering: the analyzer's handler-body rule rejects it
// (`count.focus()` is not a setter call). Lowering itself rejects arguments.
void test_ref_focus_rejects_arguments() {
    std::unique_ptr<FileFixture> keep;
    check(!lower_page(ref_page("target.focus(1);"), keep).has_value(),
          "focus() takes no arguments");
}

void test_ref_focus_codegen_uses_the_focus_manager() {
    std::unique_ptr<FileFixture> keep;
    auto lowered = lower_page(ref_page("target.focus(); target.blur();"), keep);
    check(lowered.has_value(), "the component lowers");
    if (!lowered) {
        return;
    }
    ir::IrModule module;
    module.name = lowered->name;
    module.components = std::move(lowered->components);
    module.bindings = std::move(lowered->bindings);
    module.stylesheets = std::move(lowered->stylesheets);
    const auto generated = codegen::generate(module);
    check(generated.has_value(), "codegen accepts ref.focus()");
    if (!generated) {
        return;
    }
    std::string source;
    for (const codegen::GeneratedFile& file : *generated) {
        source += file.text;
    }
    for (const char* line :
         {"void set_focus_manager(tessera::ui::FocusManager* manager)",
          "focus_manager_->request_focus(ref_target_);", "focus_manager_->clear_focus();"}) {
        check(source.find(line) != std::string::npos,
              std::string("the generated code contains ") + line);
    }
}

// --- Runtime, headless -----------------------------------------------------------------------

using tessera::ui::Event;
using tessera::ui::EventType;
using tessera::ui::FocusManager;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputRouter;
using tessera::ui::Node;

struct KeyLog {
    int key_downs = 0;
    int focuses = 0;
    int blurs = 0;
};

void on_key_down(void* context, const Event&) {
    ++static_cast<KeyLog*>(context)->key_downs;
}
void on_focus(void* context, const Event&) {
    ++static_cast<KeyLog*>(context)->focuses;
}
void on_blur(void* context, const Event&) {
    ++static_cast<KeyLog*>(context)->blurs;
}

// Built exactly as the generated builder builds a focusable View with the three bound events.
Node& add_focusable(Node& root, tessera::ui::NodeId id, int tab_index, KeyLog& log,
                    bool auto_focus = false) {
    Node& node = root.add_child(std::make_unique<Node>(id));
    node.set_element_kind("View");
    node.set_role("button");
    node.set_focusable(true);
    node.set_tab_index(tab_index);
    node.set_auto_focus(auto_focus);
    node.bind_event(EventType::KeyDown, &on_key_down, &log);
    node.bind_event(EventType::Focus, &on_focus, &log);
    node.bind_event(EventType::Blur, &on_blur, &log);
    return node;
}

InputEvent key_down(tessera::ui::KeyCode code) {
    InputEvent event;
    event.type = InputEventType::KeyDown;
    event.key = code;
    return event;
}

void test_tab_reaches_the_view_and_key_events_fire() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    KeyLog first_log;
    KeyLog second_log;
    Node& first = add_focusable(root, 2, 0, first_log);
    Node& second = add_focusable(root, 3, 0, second_log);
    InputRouter router;

    router.dispatch(root, key_down(tessera::ui::key::kTab));
    check(router.focus_manager().focused() == &first, "Tab reaches the first focusable View");
    check(first_log.focuses == 1, "onFocus fires on the focused View");
    router.dispatch(root, key_down(0x41));
    check(first_log.key_downs == 1, "onKeyDown fires on the focused View");
    check(second_log.key_downs == 0, "the other View sees no key");
    router.dispatch(root, key_down(tessera::ui::key::kTab));
    check(router.focus_manager().focused() == &second, "Tab moves to the next View");
    check(first_log.blurs == 1, "onBlur fires when focus leaves");
}

void test_minus_one_is_skipped_by_tab_but_focusable_by_request() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    KeyLog skipped_log;
    KeyLog tabbed_log;
    Node& skipped = add_focusable(root, 2, -1, skipped_log);
    Node& tabbed = add_focusable(root, 3, 0, tabbed_log);
    InputRouter router;

    router.dispatch(root, key_down(tessera::ui::key::kTab));
    check(router.focus_manager().focused() == &tabbed, "Tab skips tabIndex -1");
    router.focus_manager().set_root(root);
    check(router.focus_manager().request_focus(&skipped), "request_focus reaches a -1 View");
    check(router.focus_manager().focused() == &skipped, "the -1 View holds focus");
    check(skipped_log.focuses == 1, "onFocus fires for programmatic focus");
}

void test_auto_focus_picks_the_first_eligible_view() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    KeyLog plain_log;
    KeyLog wanted_log;
    KeyLog later_log;
    add_focusable(root, 2, 0, plain_log);
    Node& wanted = add_focusable(root, 3, 0, wanted_log, true);
    add_focusable(root, 4, 0, later_log, true);
    FocusManager manager;
    manager.set_root(root);

    check(manager.apply_auto_focus(root), "autoFocus moves focus");
    check(manager.focused() == &wanted, "the first autoFocus View in tree order is focused");
    check(!manager.apply_auto_focus(root), "autoFocus does not steal focus once held");
}

// The statements the generated handler runs for `target.focus()` / `target.blur()`, verbatim from
// emit_actions (checked textually above); `ref_target_` is the node build() assigned.
void test_ref_focus_moves_and_clears_focus() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    KeyLog target_log;
    KeyLog other_log;
    Node& other = add_focusable(root, 2, 0, other_log);
    Node* ref_target_ = &add_focusable(root, 3, -1, target_log);
    FocusManager manager;
    manager.set_root(root);
    FocusManager* focus_manager_ = &manager;

    check(manager.request_focus(&other), "focus starts on another View");
    if (focus_manager_ != nullptr && ref_target_ != nullptr) {
        focus_manager_->request_focus(ref_target_);
    }
    check(manager.focused() == ref_target_, "ref.focus() moves focus to a tabIndex -1 View");
    check(target_log.focuses == 1 && other_log.blurs == 1, "focus and blur events fire");
    if (focus_manager_ != nullptr && ref_target_ != nullptr) {
        if (focus_manager_->focused() == ref_target_) {
            focus_manager_->clear_focus();
        }
    }
    check(manager.focused() == nullptr, "ref.blur() clears focus held by the ref");
    check(target_log.blurs == 1, "onBlur fires on the ref");
}

} // namespace

int main() {
    test_ref_focus_and_blur_lower_to_actions();
    test_ref_focus_rejects_arguments();
    test_ref_focus_codegen_uses_the_focus_manager();
    test_ref_focus_moves_and_clears_focus();
    test_props_and_events_lower_to_ir();
    test_minus_one_and_text_lower();
    test_bad_values_are_rejected();
    test_focusable_needs_a_role_and_a_name();
    test_codegen_marks_the_node_and_binds_events();
    test_tab_reaches_the_view_and_key_events_fire();
    test_minus_one_is_skipped_by_tab_but_focusable_by_request();
    test_auto_focus_picks_the_first_eligible_view();
    if (jsx_test::failures != 0) {
        std::cerr << jsx_test::failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "focusable view tests passed\n";
    return 0;
}
