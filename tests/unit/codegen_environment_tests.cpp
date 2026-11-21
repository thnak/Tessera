// SPDX-License-Identifier: MIT
//
// `environment` hook codegen (docs/specs/jsx-subset-v1.md section 7.1, issue #692): a component
// that reads `useBreakpoint()` gets one private field, an equality-gated `set_environment_<name>()`
// the host calls, and an accessor the developer class reads. A component that reads none generates
// no environment surface at all, so every existing golden stays byte-identical.

#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace codegen = tessera::compiler::codegen;
namespace ir = tessera::compiler::ir;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

ir::SourceSpan span(std::uint32_t offset, std::uint32_t length) {
    ir::SourceSpan value;
    value.file = "ui/Breakpoints.tsx";
    value.offset = offset;
    value.length = length;
    value.line = 1;
    value.column = 3;
    value.end_line = 1;
    value.end_column = 3 + length;
    return value;
}

ir::Node node_of(ir::NodeKind kind, std::string id, std::string ui_id, std::uint32_t offset) {
    ir::Node node;
    node.kind = kind;
    node.id = std::move(id);
    node.ui_id = std::move(ui_id);
    node.source = span(offset, 3);
    return node;
}

// Breakpoints: `const bp = useBreakpoint();` and a Text whose content is `bp`. The text binding is
// what makes the value observable: it is the expression the generated `apply_state()` re-evaluates
// when the host pushes a new class.
ir::IrModule make_module() {
    ir::IrModule module;
    module.name = "breakpoints";

    ir::Component component;
    component.id = "comp::Breakpoints";
    component.component_id = "71";
    component.name = "Breakpoints";
    component.source = span(0, 1);
    component.environment.push_back(
        ir::Environment{"env::bp", "bp", "breakpoint", "string", false, span(10, 20)});
    component.root = node_of(ir::NodeKind::View, "node::1", "501", 30);
    ir::Node label = node_of(ir::NodeKind::Text, "node::2", "502", 40);
    label.text = ir::make_ref("env::bp");
    component.root.children.push_back(std::move(label));
    module.components.push_back(std::move(component));
    return module;
}

// A component that reads no environment value: the control for "no surface, no golden churn".
ir::IrModule make_plain_module() {
    ir::IrModule module;
    module.name = "plain";
    ir::Component component;
    component.id = "comp::Plain";
    component.component_id = "72";
    component.name = "Plain";
    component.source = span(0, 1);
    component.root = node_of(ir::NodeKind::View, "node::1", "601", 10);
    module.components.push_back(std::move(component));
    return module;
}

std::string first_error(const codegen::CodegenResult& result) {
    if (result) {
        return {};
    }
    return result.error().empty() ? std::string("<empty>") : result.error().front();
}

std::string file_text(const std::vector<codegen::GeneratedFile>& files, std::string_view path) {
    for (const codegen::GeneratedFile& file : files) {
        if (file.path == path) {
            return file.text;
        }
    }
    return {};
}

void test_fixture_is_valid() {
    std::string codes;
    for (const ir::IrDiagnostic& diagnostic : ir::validate(make_module())) {
        codes += diagnostic.code + ":" + diagnostic.message + "; ";
    }
    check(codes.empty(), "the environment fixture is a valid IR module: " + codes);
}

void test_generated_surface() {
    const auto result = codegen::generate(make_module());
    check(result.has_value(), "the environment fixture generates: " + first_error(result));
    if (!result) {
        return;
    }
    const std::string header = file_text(*result, "Breakpoints.g.hpp");
    check(!header.empty(), "the base header is generated");
    // The field, its initial value and the host-facing setter.
    check(contains(header, "std::string env_bp_ = \"compact\";"),
          "the field starts at the narrow class, the safe first frame");
    check(contains(header, "void set_environment_bp(std::string value);"),
          "the host pushes a new class through a generated setter");
    // The accessor the developer class reads.
    check(contains(header, "bp() const"),
          "the developer class reads the value through an accessor");
    // The setter body is equality-gated and schedules an update, like a state setter.
    check(contains(header, "if (env_bp_ == value)"), "the setter is equality-gated");
    check(contains(header, "schedule_update();"), "a changed class schedules an update");
    // The text binding reads the field, so the value reaches the pixels.
    check(contains(header, "env_bp_"), "the binding reads the environment field");
}

void test_no_surface_without_a_hook() {
    const auto result = codegen::generate(make_plain_module());
    check(result.has_value(), "the plain fixture generates: " + first_error(result));
    if (!result) {
        return;
    }
    const std::string header = file_text(*result, "Plain.g.hpp");
    check(!header.empty(), "the plain base header is generated");
    check(!contains(header, "set_environment_"),
          "a component that reads no environment value gets no environment surface");
    check(!contains(header, "env_"),
          "a component that reads no environment value gets no environment field");
}

} // namespace

int main() {
    test_fixture_is_valid();
    test_generated_surface();
    test_no_surface_without_a_hook();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
