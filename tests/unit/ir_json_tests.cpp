// SPDX-License-Identifier: MIT

// IR -> JSON: expression forms, omission rules, goldens and determinism.

#include "ir_samples.hpp"
#include "ir_test_util.hpp"
#include "tessera/compiler/ir/ir_json.hpp"

#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef TESSERA_IR_GOLDEN_DIR
#error "TESSERA_IR_GOLDEN_DIR must be defined by the build"
#endif

namespace {

using namespace ir_test;
using tessera::compiler::ir::to_text;

std::string expr_text(const ir::Expr* expr) {
    return to_text(ir::expr_to_json(expr));
}

std::string value_text(const ir::Expr* expr) {
    return to_text(ir::value_to_json(expr));
}

std::string flat(std::string text) {
    std::string out;
    bool skipping = false;
    for (const char c : text) {
        if (c == '\n') {
            skipping = true;
        } else if (skipping && c == ' ') {
            continue;
        } else {
            skipping = false;
            out += c;
        }
    }
    return out;
}

void test_array_expression() {
    std::vector<ir::ExprPtr> elements;
    elements.push_back(ir::make_literal(std::string("alpha")));
    elements.push_back(ir::make_literal(std::string("beta")));
    check_eq(flat(expr_text(ir::make_array(std::move(elements)).get())),
             "{\"items\": [{\"kind\": \"literal\",\"value\": \"alpha\"},"
             "{\"kind\": \"literal\",\"value\": \"beta\"}],\"kind\": \"array\"}",
             "an array writes its elements as `items` in source order");
    check_eq(flat(expr_text(ir::make_array({}).get())), "{\"items\": [],\"kind\": \"array\"}",
             "an empty array is written as an empty items list");
    // A value position writes a non-literal expression as its object (an array is not a literal).
    check_eq(flat(value_text(ir::make_array({}).get())), "{\"items\": [],\"kind\": \"array\"}",
             "an array in a value position keeps its object form");
}

void test_expression_forms() {
    check_eq(flat(expr_text(ir::make_literal(std::int64_t{42}).get())),
             "{\"kind\": \"literal\",\"value\": 42}", "int literal");
    check_eq(flat(expr_text(ir::make_literal(2.0).get())), "{\"kind\": \"literal\",\"value\": 2.0}",
             "float literal keeps its type");
    check_eq(flat(expr_text(ir::make_literal(ir::Null{}).get())),
             "{\"kind\": \"literal\",\"value\": null}", "null literal");
    check_eq(flat(expr_text(ir::make_literal(true).get())),
             "{\"kind\": \"literal\",\"value\": true}", "bool literal");
    check_eq(flat(expr_text(ir::make_ref("state::count").get())),
             "{\"id\": \"state::count\",\"kind\": \"ref\"}", "ref");
    check_eq(flat(expr_text(ir::make_binary("+", ir::make_ref("a"), ir::make_ref("b")).get())),
             "{\"kind\": \"binary\",\"left\": {\"id\": \"a\",\"kind\": \"ref\"},\"op\": \"+\","
             "\"right\": {\"id\": \"b\",\"kind\": \"ref\"}}",
             "binary keeps left and right in place");
    check_eq(flat(expr_text(ir::make_unary("!", ir::make_ref("a")).get())),
             "{\"kind\": \"unary\",\"op\": \"!\",\"value\": {\"id\": \"a\",\"kind\": \"ref\"}}",
             "unary");
    check_eq(
        flat(expr_text(
            ir::make_conditional(ir::make_ref("t"), ir::make_ref("y"), ir::make_ref("n")).get())),
        "{\"else\": {\"id\": \"n\",\"kind\": \"ref\"},\"kind\": \"conditional\","
        "\"test\": {\"id\": \"t\",\"kind\": \"ref\"},\"then\": {\"id\": \"y\",\"kind\": \"ref\"}}",
        "conditional");
    std::vector<ir::ExprPtr> args;
    args.push_back(ir::make_ref("x"));
    args.push_back(ir::make_ref("y"));
    check_eq(flat(expr_text(ir::make_call("min", std::move(args)).get())),
             "{\"args\": [{\"id\": \"x\",\"kind\": \"ref\"},{\"id\": \"y\",\"kind\": \"ref\"}],"
             "\"callee\": \"min\",\"kind\": \"call\"}",
             "call arguments stay in order");
    std::vector<ir::TemplatePart> parts;
    parts.push_back(ir::text_part("a"));
    parts.push_back(ir::expr_part(ir::make_ref("x")));
    parts.push_back(ir::text_part(""));
    check_eq(flat(expr_text(ir::make_template(std::move(parts)).get())),
             "{\"kind\": \"template\",\"parts\": [\"a\",{\"id\": \"x\",\"kind\": \"ref\"},\"\"]}",
             "template parts: text is a bare string, expressions are objects");
    check_eq(expr_text(nullptr), "null\n", "a missing expression is null");
}

void test_value_position_is_bare_for_literals_only() {
    check_eq(value_text(ir::make_literal(std::int64_t{0}).get()), "0\n", "bare int");
    check_eq(value_text(ir::make_literal(std::string("x")).get()), "\"x\"\n", "bare string");
    check_eq(value_text(ir::make_literal(1.5).get()), "1.5\n", "bare float");
    check_eq(flat(value_text(ir::make_ref("a").get())), "{\"id\": \"a\",\"kind\": \"ref\"}",
             "a ref stays an object");
    check_eq(flat(value_text(ir::make_unary("-", ir::make_literal(std::int64_t{1})).get())),
             "{\"kind\": \"unary\",\"op\": \"-\",\"value\": {\"kind\": \"literal\",\"value\": 1}}",
             "operands of an expression stay objects");
}

void test_malformed_ir_is_written_with_gaps() {
    auto broken = ir::make_binary("+", ir::make_ref("a"), nullptr);
    check_eq(flat(expr_text(broken.get())),
             "{\"kind\": \"binary\",\"left\": {\"id\": \"a\",\"kind\": \"ref\"},\"op\": \"+\","
             "\"right\": null}",
             "a null operand is null");
    auto missing = std::make_unique<ir::Expr>();
    missing->kind = ir::ExprKind::Conditional;
    check_eq(flat(expr_text(missing.get())),
             "{\"else\": null,\"kind\": \"conditional\",\"test\": null,\"then\": null}",
             "absent operands are null");
}

void test_empty_optional_fields_are_omitted() {
    ir::IrModule module;
    module.name = "m";
    check_eq(ir::write_ir_json(module),
             "{\n  \"bindings\": [],\n  \"components\": [],\n  \"irVersion\": 1,\n"
             "  \"module\": {\n    \"name\": \"m\"\n  },\n  \"stylesheets\": []\n}\n",
             "the three top-level lists are always written, module.imports is omitted");

    ir::Node node;
    node.id = "node::1";
    node.ui_id = "1";
    check_eq(flat(to_text(ir::to_json(node))),
             "{\"id\": \"node::1\",\"kind\": \"View\",\"source\": {\"column\": 1,\"endColumn\": 1,"
             "\"endLine\": 1,\"file\": \"\",\"length\": 0,\"line\": 1,\"offset\": 0},"
             "\"uiId\": \"1\"}",
             "a bare node has no props, style, events, children, condition or loop");
}

void test_new_v1_fields() {
    ir::IrModule module = make_rich_module();
    const std::string text = ir::write_ir_json(module);
    const auto has = [&](const std::string& needle) {
        return text.find(needle) != std::string::npos;
    };
    check(has("\"kind\": \"Component\""), "Component node kind");
    check(has("\"component\": \"comp::Row\""), "Component node reference");
    check(has("\"handlers\": ["), "handlers");
    check(has("\"animate\": false"), "animSet with animate false");
    check(!has("\"durationMs\"") && !has("\"easing\""), "timing is omitted when absent");
    check(has("\"returnType\": \"bool\""), "binding returnType");
    check(has("\"preview\": {"), "binding preview");
    check(has("\"loop\": {"), "loop on the node");
    check(!has("\"body\""), "loop has no body");
    check(has("\"writable\": true"), "writable environment");

    // animate is written only when false; a plain set never writes it.
    module.components[0].handlers[0].actions[1].animate = true;
    check(ir::write_ir_json(module).find("\"animate\"") == std::string::npos,
          "animate true is the default and is omitted");
    module.components[0].environment[0].writable = false;
    check(ir::write_ir_json(module).find("\"writable\"") == std::string::npos,
          "writable false is omitted");

    // A plain `set` never carries `animate`, even if the field was changed.
    module.components[0].handlers[0].actions[0].animate = false;
    check(ir::write_ir_json(module).find("\"animate\"") == std::string::npos,
          "animate is only written for animSet");
    module.components[0].handlers[0].actions[1].animate = false;
    check(ir::write_ir_json(module).find("\"animate\": false") != std::string::npos,
          "animSet with animate false writes it");

    // Timing options are written for animSet only, in a fixed order after `animate`.
    module.components[0].handlers[0].actions[1].duration_ms = 250.0;
    module.components[0].handlers[0].actions[1].easing = "ease-out";
    module.components[0].handlers[0].actions[0].duration_ms = 99.0;
    const std::string timed = ir::write_ir_json(module);
    check(timed.find("\"durationMs\": 250") != std::string::npos &&
              timed.find("\"easing\": \"ease-out\"") != std::string::npos,
          "animSet timing is written");
    check(timed.find("99") == std::string::npos, "a plain set never writes timing");

    // `index` appears only when the loop has one.
    auto& row = module.components[0].root.children[0];
    row.loop->index.clear();
    check(ir::write_ir_json(module).find("\"index\"") == std::string::npos,
          "a loop without an index parameter has no index field");

    // An effect with no dependencies still writes `deps: []` (it runs once).
    module.components[0].effects[0].deps.clear();
    check(ir::write_ir_json(module).find("\"deps\": []") != std::string::npos,
          "empty deps is written");
    module.components[0].effects[0].cleanup.clear();
    check(ir::write_ir_json(module).find("\"cleanup\"") == std::string::npos,
          "no cleanup, no field");
}

void test_custom_element_and_events() {
    ir::Node node;
    node.kind = ir::NodeKind::CustomElement;
    node.id = "node::1";
    node.ui_id = "1";
    node.element = "Waterfall3D";
    node.props.emplace("intensity", ir::make_literal(0.8));
    node.events.emplace("onSelect", "bind::on_select");
    node.events.emplace("onPointer", "handler::2");
    const std::string text = to_text(ir::to_json(node));
    check(text.find("\"element\": \"Waterfall3D\"") != std::string::npos, "element id");
    check(text.find("\"kind\": \"CustomElement\"") != std::string::npos, "kind name");
    check(text.find("\"intensity\": 0.8") != std::string::npos, "bare float prop");
    check(text.find("\"onPointer\": \"handler::2\",\n    \"onSelect\": \"bind::on_select\"") !=
              std::string::npos,
          "events are written sorted under `events`");
    check(text.find("\"events\": {") != std::string::npos, "events object");
}

void test_goldens() {
    const std::string dir = TESSERA_IR_GOLDEN_DIR;
    const std::string counter = read_file(dir + "/counter.json");
    check(!counter.empty(), "counter golden is readable");
    check_eq(ir::write_ir_json(make_counter_module()), counter, "Counter matches its golden");
    const std::string rich = read_file(dir + "/rich.json");
    check(!rich.empty(), "rich golden is readable");
    check_eq(ir::write_ir_json(make_rich_module()), rich, "rich module matches its golden");
}

void test_output_is_deterministic() {
    check_eq(ir::write_ir_json(make_rich_module()), ir::write_ir_json(make_rich_module()),
             "two builds of the same IR give the same bytes");
    ir::IrModule module = make_rich_module();
    check_eq(ir::write_ir_json(module), ir::write_ir_json(module),
             "writing does not change the IR");
    const std::string text = ir::write_ir_json(module);
    check(!text.empty() && text.back() == '\n' && text.find('\r') == std::string::npos,
          "LF only, trailing newline");
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--write") {
        // Deliberate golden update: run with --write after an intentional writer change, then
        // review the diff against docs/specs/ir-v1.md before committing.
        const std::string dir = TESSERA_IR_GOLDEN_DIR;
        const auto write = [&](const std::string& name, const std::string& text) {
            std::ofstream out(dir + "/" + name, std::ios::binary | std::ios::trunc);
            out << text;
            check(static_cast<bool>(out), "wrote " + name);
        };
        write("counter.json", ir::write_ir_json(make_counter_module()));
        write("rich.json", ir::write_ir_json(make_rich_module()));
        return finish("ir json golden update");
    }
    test_expression_forms();
    test_array_expression();
    test_value_position_is_bare_for_literals_only();
    test_malformed_ir_is_written_with_gaps();
    test_empty_optional_fields_are_omitted();
    test_new_v1_fields();
    test_custom_element_and_events();
    test_goldens();
    test_output_is_deterministic();
    return finish("ir json tests");
}
