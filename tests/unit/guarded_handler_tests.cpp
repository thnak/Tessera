// SPDX-License-Identifier: MIT

// Guarded handler statements (issue #236): `e.key === "Escape" && setOpen(false)` runs the setter
// only when the pure guard holds. Three layers: lowering puts the guard on the IR action,
// the evaluator skips the action when the guard is false, and codegen wraps it in an `if`.

#include "lowering_module_test_util.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/eval/evaluator.hpp"
#include "tessera/ui/scene/event_payloads.hpp"

#include <memory>
#include <string>
#include <utility>

namespace {

using namespace lowering_module_test;
namespace codegen = tessera::compiler::codegen;
namespace eval = tessera::compiler::eval;

const std::string kPage =
    "import { useState, KeyEvent } from \"tessera\";\n"
    "function Menu() {\n"
    "  const [open, setOpen] = useState<boolean>(true);\n"
    "  return (\n"
    "    <View role=\"menu\" tabIndex={0} aria-label=\"Menu\"\n"
    "      onKeyDown={(e: KeyEvent) => e.key === \"Escape\" && setOpen(false)}>\n"
    "      <Text>{open ? \"open\" : \"closed\"}</Text>\n"
    "    </View>\n"
    "  );\n"
    "}\n";

lowering::ModuleLoweringResult lower_menu(std::unique_ptr<FileFixture>& keep,
                                          const std::string& text = kPage) {
    keep = parse_file("ui/Menu.tsx", text);
    return lowering::lower_module(keep->module_source(), "demo");
}

const ir::Handler* only_handler(const lowering::LoweredModule& module) {
    const ir::Component* component = find_component(module, "Menu");
    return component != nullptr && component->handlers.size() == 1 ? &component->handlers[0]
                                                                   : nullptr;
}

void test_guard_lowers_onto_the_action() {
    std::unique_ptr<FileFixture> keep;
    const auto result = lower_menu(keep);
    check(result.has_value(), "a guarded handler lowers: " + first_message(result));
    if (!result) {
        return;
    }
    const ir::Handler* handler = only_handler(*result);
    check(handler != nullptr && handler->actions.size() == 1, "one inline handler, one action");
    if (handler == nullptr || handler->actions.size() != 1) {
        return;
    }
    const ir::Action& action = handler->actions[0];
    check(action.guard != nullptr && action.guard->kind == ir::ExprKind::Binary,
          "the action carries the guard");
    check_eq(action.target, "state::open", "the setter target is kept");
}

void test_evaluator_skips_a_false_guard() {
    std::unique_ptr<FileFixture> keep;
    const auto result = lower_menu(keep);
    const ir::Handler* handler = result ? only_handler(*result) : nullptr;
    check(handler != nullptr, "the handler lowers");
    if (handler == nullptr) {
        return;
    }
    for (const auto& [key, expected] :
         {std::pair<const char*, bool>{"a", true}, std::pair<const char*, bool>{"Escape", false}}) {
        eval::Scope scope;
        scope["state::open"] = eval::Value::of_bool(true);
        scope["e.key"] = eval::Value::of_string(key);
        eval::Evaluator evaluator;
        const auto ran = evaluator.run(handler->actions, scope);
        check(ran.has_value(), std::string("the handler runs for key ") + key);
        const bool* open = scope["state::open"].as_bool();
        check(open != nullptr && *open == expected,
              std::string("key ") + key + (expected ? " leaves the state" : " closes the menu"));
    }
}

void test_codegen_wraps_the_action_in_an_if() {
    std::unique_ptr<FileFixture> keep;
    auto lowered = lower_menu(keep);
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
    check(generated.has_value(),
          "codegen accepts a guarded handler: " +
              (generated || generated.error().empty() ? std::string() : generated.error().front()));
    if (!generated) {
        return;
    }
    std::string source;
    for (const codegen::GeneratedFile& file : *generated) {
        source += file.text;
    }
    check(source.find("tessera::ui::key_is(e, \"Escape\")") != std::string::npos &&
              source.find("if (") != std::string::npos,
          "the generated handler tests the key before assigning");
}

void test_unguarded_non_setter_is_still_rejected() {
    std::unique_ptr<FileFixture> keep;
    std::string text = kPage;
    text.replace(text.find("setOpen(false)"), 14, "console.log(1)");
    check(!lower_menu(keep, text).has_value(), "a guard on a non-setter call is rejected");
}

void test_key_is_maps_w3c_names_to_codes() {
    using tessera::ui::KeyEventInfo;
    namespace key = tessera::ui::key;
    check(tessera::ui::key_is(KeyEventInfo{key::kEscape}, "Escape"), "Escape");
    check(tessera::ui::key_is(KeyEventInfo{key::kArrowDown}, "ArrowDown"), "ArrowDown");
    check(tessera::ui::key_is(KeyEventInfo{key::kSpace}, " "), "Space is the string \" \"");
    check(tessera::ui::key_is(KeyEventInfo{'A'}, "a"), "a letter matches either case");
    check(!tessera::ui::key_is(KeyEventInfo{key::kEnter}, "Escape"), "a different key");
    check(!tessera::ui::key_is(KeyEventInfo{key::kEnter}, "Bogus"), "an unknown name");
}

} // namespace

int main() {
    test_key_is_maps_w3c_names_to_codes();
    test_guard_lowers_onto_the_action();
    test_evaluator_skips_a_false_guard();
    test_codegen_wraps_the_action_in_an_if();
    test_unguarded_non_setter_is_still_rejected();
    if (jsx_test::failures != 0) {
        std::cerr << jsx_test::failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "guarded handler tests passed\n";
    return 0;
}
