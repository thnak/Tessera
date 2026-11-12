// SPDX-License-Identifier: MIT

// IR validation (docs/specs/ir-v1.md section 9): one valid module, then one broken copy per rule.

#include "ir_samples.hpp"
#include "ir_test_util.hpp"
#include "ir_validate_util.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ir_test;

void test_valid_module_and_purity() {
    const ir::IrModule module = make_valid_module();
    check_eq(codes_of(module), "", "the sample module is valid");
    check_eq(codes_of(make_valid_module()), codes_of(module), "validation is repeatable");
    const std::string before = ir::write_ir_json(module);
    ir::validate(module);
    check_eq(ir::write_ir_json(module), before, "validation does not change the module");
    ir::IrModule empty;
    check_eq(codes_of(empty), "", "an empty module is valid");
}

void test_version_and_identity() {
    expect("unsupported version", [](auto& m) { m.ir_version = 2; }, "TESSERA_IR_VERSION;");
    expect(
        "duplicate component id", [](auto& m) { m.components[1].id = "comp::List"; },
        std::string(kDuplicate) + kUnresolved);
    expect("duplicate node id", [](auto& m) { label_node(m).id = row_node(m).id; }, kDuplicate);
    expect(
        "duplicate uiId in another component", [](auto& m) { m.components[1].root.ui_id = "1"; },
        kDuplicate);
    expect(
        "duplicate uiId in one component", [](auto& m) { label_node(m).ui_id = row_node(m).ui_id; },
        kDuplicate);
    expect("missing uiId", [](auto& m) { label_node(m).ui_id.clear(); }, kMissing);
    expect("missing node id", [](auto& m) { label_node(m).id.clear(); }, kMissing);
    expect("missing componentId", [](auto& m) { list(m).component_id.clear(); }, kMissing);
    expect(
        "duplicate componentId", [](auto& m) { m.components[1].component_id = "42"; }, kDuplicate);
    expect(
        "automationId differs from uiId", [](auto& m) { label_node(m).automation_id = "99"; },
        "TESSERA_IR_AUTOMATION_ID;");
    expect(
        "automationId equal to uiId is fine",
        [](auto& m) { label_node(m).automation_id = label_node(m).ui_id; }, "");
    expect(
        "component without a name", [](auto& m) { m.components[1].name.clear(); },
        "TESSERA_IR_ORDER;" + std::string(kMalformed));
}

void test_declarations() {
    expect(
        "duplicate state id",
        [](auto& m) {
            list(m).state.push_back(
                {"state::rows", "rows", "int", ir::make_literal(std::int64_t{0})});
        },
        kDuplicate);
    expect(
        "state id without prefix",
        [](auto& m) {
            list(m).state.push_back({"x", "x", "int", ir::make_literal(std::int64_t{0})});
        },
        kMalformed);
    expect(
        "duplicate prop", [](auto& m) { list(m).props.push_back({"start", "int", nullptr}); },
        kDuplicate);
    expect(
        "duplicate handler id",
        [](auto& m) {
            list(m).handlers.push_back(ir::Handler{"handler::1", {}, {}, span(1, 1, 1, 1, 1, 2)});
        },
        kDuplicate);
    expect(
        "handler id format",
        [](auto& m) {
            list(m).handlers[0].id = "h1";
            row_node(m).events["onPick"] = "h1";
        },
        kMalformed + std::string(kMalformed));
    expect("state without initial", [](auto& m) { list(m).state[0].initial.reset(); }, kMalformed);
    expect(
        "environment without provider", [](auto& m) { list(m).environment[0].provider.clear(); },
        kMalformed);
    expect(
        "effect on a missing dependency",
        [](auto& m) { list(m).effects[0].deps.push_back("state::gone"); }, kUnresolved);
    expect(
        "effect cannot depend on a ref",
        [](auto& m) { list(m).effects[0].deps.push_back("ref::box"); }, kUnresolved);
    expect(
        "effect may depend on a prop",
        [](auto& m) { list(m).effects[0].deps.push_back("prop::start"); }, "");
    expect(
        "effect cannot depend on an environment value",
        [](auto& m) { list(m).effects[0].deps.push_back("env::theme"); }, kUnresolved);
    expect(
        "effect handler binding of another component",
        [](auto& m) { m.bindings[1].component = "comp::Row"; },
        std::string(kUnresolved) + "TESSERA_IR_BINDING;");
    expect(
        "prop default is checked",
        [](auto& m) { list(m).props[0].default_value = ir::make_ref("zzz"); }, kUnresolved);
    expect(
        "ref to a missing node", [](auto& m) { list(m).refs[0].target = "node::99"; }, kUnresolved);
}

void test_references() {
    const auto set_text = [](ir::IrModule& m, ir::ExprPtr text) {
        label_node(m).text = std::move(text);
    };
    expect(
        "unknown state", [&](auto& m) { set_text(m, ir::make_ref("state::gone")); }, kUnresolved);
    expect("unknown prop", [&](auto& m) { set_text(m, ir::make_ref("prop::gone")); }, kUnresolved);
    expect(
        "known prop with a field", [&](auto& m) { set_text(m, ir::make_ref("prop::title.x")); },
        "");
    expect(
        "field of an unknown base", [&](auto& m) { set_text(m, ir::make_ref("state::gone.x")); },
        kUnresolved);
    expect(
        "bare name outside any loop", [&](auto& m) { set_text(m, ir::make_ref("item.id")); },
        kUnresolved);
    expect(
        "loop item inside the loop node",
        [&](auto& m) { row_node(m).props.emplace("x", ir::make_ref("index")); }, "");
    expect(
        "loop source cannot use its own item",
        [&](auto& m) { row_node(m).loop->source = ir::make_ref("item.rows"); }, kUnresolved);
    expect(
        "an env reference resolves", [&](auto& m) { set_text(m, ir::make_ref("env::theme")); }, "");
    expect(
        "an animated reference resolves", [&](auto& m) { set_text(m, ir::make_ref("anim::fade")); },
        "");
    expect("a ref hook resolves", [&](auto& m) { set_text(m, ir::make_ref("ref::box")); }, "");
    expect(
        "action target is not state",
        [](auto& m) { list(m).handlers[0].actions[0].target = "anim::fade"; }, kUnresolved);
    expect(
        "animSet target is not animated",
        [](auto& m) { list(m).handlers[0].actions[1].target = "state::n"; }, kUnresolved);
    expect(
        "set action on an undeclared state variable",
        [](auto& m) { list(m).handlers[0].actions[0].target = "state::gone"; }, kUnresolved);
    expect(
        "animSet action on an undeclared animated value",
        [](auto& m) { list(m).handlers[0].actions[1].target = "anim::gone"; }, kUnresolved);
    expect(
        "action reads a parameter",
        [](auto& m) { list(m).handlers[0].actions[0].value = ir::make_ref("e"); }, "");
    expect(
        "action reads an unknown name",
        [](auto& m) { list(m).handlers[0].actions[0].value = ir::make_ref("zzz"); }, kUnresolved);
    expect(
        "action without a value", [](auto& m) { list(m).handlers[0].actions[0].value.reset(); },
        kMalformed);

    // animSet timing options: valid forms pass, contradictions and bad values are
    // TESSERA_IR_ANIMATION.
    const auto timing = [](auto& m, std::optional<double> duration, std::string easing) {
        ir::Action& action = list(m).handlers[0].actions[1];
        action.animate = true;
        action.duration_ms = duration;
        action.easing = std::move(easing);
    };
    constexpr const char* kAnimation = "TESSERA_IR_ANIMATION;";
    expect(
        "animSet timing is valid",
        [&](auto& m) { timing(m, 300.0, "cubic-bezier(0.4, 0, 0.2, 1)"); }, "");
    expect("animSet keyword easing is valid", [&](auto& m) { timing(m, 0.0, "ease-in-out"); }, "");
    expect("animSet duration above the cap", [&](auto& m) { timing(m, 60001.0, ""); }, kAnimation);
    expect("animSet negative duration", [&](auto& m) { timing(m, -1.0, ""); }, kAnimation);
    expect(
        "animSet unknown easing", [&](auto& m) { timing(m, std::nullopt, "bounce"); }, kAnimation);
    expect(
        "animSet cubic-bezier x out of range",
        [&](auto& m) { timing(m, std::nullopt, "cubic-bezier(1.5, 0, 0.2, 1)"); }, kAnimation);
    expect(
        "animSet cubic-bezier needs four numbers",
        [&](auto& m) { timing(m, std::nullopt, "cubic-bezier(0, 0, 1)"); }, kAnimation);
    expect(
        "animate false cannot take timing",
        [&](auto& m) {
            timing(m, 100.0, "");
            list(m).handlers[0].actions[1].animate = false;
        },
        kAnimation);
    expect(
        "timing on a plain set",
        [](auto& m) { list(m).handlers[0].actions[0].duration_ms = 100.0; }, kAnimation);

    const auto spring = [](auto& m, ir::SpringTiming timing) {
        ir::Action& action = list(m).handlers[0].actions[1];
        action.animate = true;
        action.spring = timing;
    };
    expect("animSet spring is valid", [&](auto& m) { spring(m, {300.0, 0.0, 2.0}); }, "");
    expect(
        "spring with a duration",
        [&](auto& m) {
            spring(m, {});
            list(m).handlers[0].actions[1].duration_ms = 100.0;
        },
        kAnimation);
    expect(
        "spring with an easing",
        [&](auto& m) {
            spring(m, {});
            list(m).handlers[0].actions[1].easing = "linear";
        },
        kAnimation);
    expect(
        "spring with animate false",
        [&](auto& m) {
            spring(m, {});
            list(m).handlers[0].actions[1].animate = false;
        },
        kAnimation);
    expect(
        "spring on a plain set",
        [](auto& m) { list(m).handlers[0].actions[0].spring = ir::SpringTiming{}; }, kAnimation);
    expect("spring stiffness zero", [&](auto& m) { spring(m, {0.0, 20.0, 1.0}); }, kAnimation);
    expect(
        "spring stiffness too large", [&](auto& m) { spring(m, {100001.0, 20.0, 1.0}); },
        kAnimation);
    expect("spring damping negative", [&](auto& m) { spring(m, {180.0, -1.0, 1.0}); }, kAnimation);
    expect(
        "spring damping too large", [&](auto& m) { spring(m, {180.0, 10001.0, 1.0}); }, kAnimation);
    expect("spring mass zero", [&](auto& m) { spring(m, {180.0, 20.0, 0.0}); }, kAnimation);
    expect("spring mass too large", [&](auto& m) { spring(m, {180.0, 20.0, 1001.0}); }, kAnimation);
    expect(
        "spring stiffness not finite",
        [&](auto& m) { spring(m, {std::numeric_limits<double>::infinity(), 20.0, 1.0}); },
        kAnimation);
}

void test_events_and_bindings() {
    expect(
        "unknown binding", [](auto& m) { label_node(m).events["onTap"] = "bind::none"; },
        std::string(kUnresolved) + "TESSERA_IR_BINDING;");
    expect(
        "unknown handler", [](auto& m) { row_node(m).events["onPick"] = "handler::9"; },
        kUnresolved);
    expect(
        "event target of no known kind", [](auto& m) { row_node(m).events["onPick"] = "plain"; },
        kMalformed);
    expect(
        "binding of another component",
        [](auto& m) {
            m.bindings[0].component = "comp::Row";
            m.bindings[0].preview.reset();
        },
        std::string(kUnresolved) + "TESSERA_IR_BINDING;");
    expect(
        "effect with a missing handler binding",
        [](auto& m) { list(m).effects[0].handler = "bind::none"; },
        std::string(kUnresolved) + "TESSERA_IR_BINDING;");
    expect(
        "effect with a missing cleanup binding",
        [](auto& m) { list(m).effects[0].cleanup = "bind::none"; },
        std::string(kUnresolved) + "TESSERA_IR_BINDING;");
    expect(
        "binding id without prefix",
        [](auto& m) {
            m.bindings[0].id = "on_pick";
            label_node(m).events["onTap"] = "bind::on_pick";
        },
        "TESSERA_IR_UNRESOLVED;TESSERA_IR_BINDING;TESSERA_IR_BINDING;TESSERA_IR_ORDER;");
    expect(
        "region differs from the id", [](auto& m) { m.bindings[0].region = "other"; },
        "TESSERA_IR_BINDING;");
    expect(
        "binding nobody uses", [](auto& m) { label_node(m).events.clear(); },
        "TESSERA_IR_BINDING;");
    expect(
        "duplicate binding id",
        [](auto& m) {
            m.bindings.insert(m.bindings.begin() + 1, ir::Binding{});
            m.bindings[1].id = "bind::on_pick";
            m.bindings[1].component = "comp::List";
            m.bindings[1].region = "on_pick";
            m.bindings[1].source = span(1, 1, 1, 1, 1, 2);
        },
        "TESSERA_IR_DUPLICATE_ID;TESSERA_IR_ORDER;");
    expect(
        "preview reads a parameter",
        [](auto& m) { m.bindings[0].preview->actions[0].value = ir::make_ref("label"); }, "");
    expect(
        "preview reads an unknown name",
        [](auto& m) { m.bindings[0].preview->actions[0].value = ir::make_ref("zzz"); },
        kUnresolved);
    expect(
        "preview return reads an unknown name",
        [](auto& m) { m.bindings[0].preview->return_value = ir::make_ref("zzz"); }, kUnresolved);
    expect(
        "binding of an unknown component with a preview",
        [](auto& m) { m.bindings[0].component = "comp::Gone"; },
        std::string(kUnresolved) + "TESSERA_IR_BINDING;" + kUnresolved);
    expect(
        "binding parameter type", [](auto& m) { m.bindings[0].params[0].type = "1x"; },
        kUnsupported);
    expect(
        "binding return type", [](auto& m) { m.bindings[0].return_type = "list<"; }, kUnsupported);
}

} // namespace

int main() {
    test_valid_module_and_purity();
    test_version_and_identity();
    test_declarations();
    test_references();
    test_events_and_bindings();
    return finish("ir validate tests");
}
