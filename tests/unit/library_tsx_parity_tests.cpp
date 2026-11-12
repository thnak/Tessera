// SPDX-License-Identifier: MIT

// `Field` and `Form` are authored as real TSX sources (tessera/components/Field.tsx and
// tessera/components/Form.tsx, issue #70). The build lowers them with `tessera_codegen
// --emit-library-cpp` and compiles the generated `library_components_generated.cpp` into
// `tessera_lowering`; the TSX sources are the single source of truth.
//
// This test is the staleness guard: it re-runs the ordinary parser, analysis and lowering pipeline
// on the committed TSX in-process and compares the result with `make_library_component()` (the
// generated declaration compiled into the test binary). An edit to a source file, the lowering
// pipeline or the generated data that is not followed by a rebuild fails here. The generated data
// differs from a raw lowering in exactly one respect: the build generator stamps the stable
// component/node identity (`Component::component_id`, `Node::ui_id`) from the fixed library salt
// (docs/specs/automation-ids-v1.md section 2.1) that the committed codegen goldens pin. The
// comparison clears those two fields and compares everything else byte for byte, including source
// spans, prop defaults and the lowered text-run shape.

#include "lowering_module_test_util.hpp"
#include "tessera/compiler/lowering/library_components.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#ifndef TESSERA_LIBRARY_TSX_DIR
#error "TESSERA_LIBRARY_TSX_DIR must be defined by the build"
#endif

namespace {

using namespace lowering_module_test;
namespace fs = std::filesystem;

constexpr std::string_view kFieldFile = "tessera/components/Field.tsx";
constexpr std::string_view kFormFile = "tessera/components/Form.tsx";

std::string source_path(std::string_view file) {
    return (fs::path(TESSERA_LIBRARY_TSX_DIR) / fs::path(file).filename()).string();
}

std::unique_ptr<FileFixture> parse_source_file(std::string_view file) {
    const std::string path = source_path(file);
    std::ifstream in(path, std::ios::binary);
    check(in.good(), "the TSX source exists: " + path);
    if (!in) {
        return nullptr;
    }
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    // The project-relative path, not the build path: it is what the IR records as `source.file`.
    return parse_file(std::string(file), std::move(text));
}

// ---------------------------------------------------------------------------------------------
// Deep copies, so normalization never mutates the lowering result or the generated declaration.

ir::ExprPtr clone_expr(const ir::Expr* expr) {
    if (expr == nullptr) {
        return nullptr;
    }
    auto copy = std::make_unique<ir::Expr>();
    copy->kind = expr->kind;
    copy->literal = expr->literal;
    copy->id = expr->id;
    copy->op = expr->op;
    for (const ir::ExprPtr& operand : expr->operands) {
        copy->operands.push_back(clone_expr(operand.get()));
    }
    for (const ir::TemplatePart& part : expr->parts) {
        copy->parts.push_back(part.expr ? ir::expr_part(clone_expr(part.expr.get()))
                                        : ir::text_part(part.text));
    }
    return copy;
}

ir::Node clone_node(const ir::Node& node) {
    ir::Node copy;
    copy.kind = node.kind;
    copy.id = node.id;
    copy.ui_id = node.ui_id;
    copy.automation_id = node.automation_id;
    copy.source = node.source;
    copy.element = node.element;
    copy.component = node.component;
    copy.key = clone_expr(node.key.get());
    for (const auto& entry : node.props) {
        copy.props.emplace(entry.first, clone_expr(entry.second.get()));
    }
    for (const auto& entry : node.style) {
        copy.style.emplace(entry.first, clone_expr(entry.second.get()));
    }
    copy.events = node.events;
    copy.text = clone_expr(node.text.get());
    for (const ir::Node& child : node.children) {
        copy.children.push_back(clone_node(child));
    }
    copy.condition = clone_expr(node.condition.get());
    if (node.loop) {
        ir::Loop loop;
        loop.item = node.loop->item;
        loop.index = node.loop->index;
        loop.source = clone_expr(node.loop->source.get());
        copy.loop = std::move(loop);
    }
    if (node.fallback) {
        copy.fallback = std::make_unique<ir::Node>(clone_node(*node.fallback));
    }
    if (node.item_template) {
        copy.item_template = std::make_unique<ir::Node>(clone_node(*node.item_template));
    }
    return copy;
}

// Field/Form are presentational: the tree and the prop records are the whole component. Checked
// before the copy drops those vectors, so a source edit that adds state or a handler fails loudly.
bool is_presentational(const ir::Component& component) {
    return component.state.empty() && component.handlers.empty() && component.effects.empty() &&
           component.refs.empty() && component.animated_values.empty() &&
           component.environment.empty();
}

ir::Component clone_component(const ir::Component& component) {
    ir::Component copy;
    copy.id = component.id;
    copy.component_id = component.component_id;
    copy.name = component.name;
    copy.source = component.source;
    for (const ir::Prop& prop : component.props) {
        ir::Prop entry;
        entry.name = prop.name;
        entry.type = prop.type;
        entry.optional = prop.optional;
        entry.default_value = clone_expr(prop.default_value.get());
        copy.props.push_back(std::move(entry));
    }
    copy.root = clone_node(component.root);
    return copy;
}

// The fixed library identity is the only thing the build generator adds.
void normalize_identity_nodes(ir::Node& node) {
    node.ui_id.clear();
    for (ir::Node& child : node.children) {
        normalize_identity_nodes(child);
    }
    if (node.fallback) {
        normalize_identity_nodes(*node.fallback);
    }
    if (node.item_template) {
        normalize_identity_nodes(*node.item_template);
    }
}

void normalize_identity(ir::Component& component) {
    component.component_id.clear();
    normalize_identity_nodes(component.root);
}

// ---------------------------------------------------------------------------------------------
// The declared TSX prop records (the type alias annotation of the component's props parameter).

struct DeclaredRecord {
    std::string name;
    std::string type;
    bool optional = false;
};

const jsx::ModuleItem* find_item(const jsx::Module& module, jsx::ItemKind kind,
                                 std::string_view name) {
    for (const jsx::ModuleItem& item : module.items) {
        if (item.kind == kind && item.name == name) {
            return &item;
        }
    }
    return nullptr;
}

// The source type -> IR prop type mapping of forms sections 5.1/5.2. It is duplicated on purpose:
// this test pins the documented mapping instead of assuming the lowering table.
std::string ir_type_of(const jsx::TypeNode& type) {
    switch (type.kind) {
    case jsx::TypeKind::Name:
        if (type.text == "string") {
            return "string";
        }
        if (type.text == "boolean") {
            return "bool";
        }
        if (type.text == "TextChangeEvent") {
            return "TextChangeEvent"; // the payload record of a callback prop (forms 5.1)
        }
        if (type.text == "Children") {
            return "children";
        }
        return {};
    case jsx::TypeKind::Array:
        if (type.children.size() == 1) {
            const std::string element = ir_type_of(*type.children[0]);
            if (element == "string" || element == "int" || element == "float" ||
                element == "bool") {
                return "list<" + element + ">";
            }
        }
        return {};
    case jsx::TypeKind::Function:
        // `() => void` is the payload-free `callback` prop type (forms 5.2).
        if (type.children.size() == 1 && type.children[0]->kind == jsx::TypeKind::Name &&
            type.children[0]->text == "void") {
            return "callback";
        }
        // `(event: TextChangeEvent) => void` would be the function spelling of the same record.
        if (type.children.size() == 2 && type.param_names.size() == 1 &&
            type.children[1]->kind == jsx::TypeKind::Name && type.children[1]->text == "void") {
            return ir_type_of(*type.children[0]);
        }
        return {};
    case jsx::TypeKind::StringLiteral:
    case jsx::TypeKind::Union:
    case jsx::TypeKind::Object:
        return {};
    }
    return {};
}

std::vector<DeclaredRecord> declared_records(const jsx::Module& module,
                                             const jsx::ModuleItem& component) {
    std::vector<DeclaredRecord> records;
    if (!component.props_type || component.props_type->kind != jsx::TypeKind::Name) {
        return records;
    }
    const jsx::ModuleItem* alias =
        find_item(module, jsx::ItemKind::TypeAlias, component.props_type->text);
    if (alias == nullptr || !alias->type || alias->type->kind != jsx::TypeKind::Object) {
        return records;
    }
    for (const jsx::TypeMember& member : alias->type->members) {
        records.push_back(DeclaredRecord{
            member.name, member.type ? ir_type_of(*member.type) : std::string(), member.optional});
    }
    return records;
}

void check_declared_records(const jsx::Module& module, const jsx::ModuleItem& component,
                            const ir::Component& generated, std::string_view name) {
    const std::vector<DeclaredRecord> records = declared_records(module, component);
    check(records.size() == generated.props.size(),
          std::string(name) + ": the TSX alias declares one record per generated prop, found " +
              std::to_string(records.size()) + " and " + std::to_string(generated.props.size()));
    const std::size_t count = std::min(records.size(), generated.props.size());
    for (std::size_t i = 0; i < count; ++i) {
        const std::string where = std::string(name) + " prop " + std::to_string(i + 1);
        const DeclaredRecord& record = records[i];
        const ir::Prop& prop = generated.props[i];
        check_eq(record.name, prop.name, where + ": the TSX record name matches the IR record");
        check(!record.type.empty(),
              where + " (`" + record.name + "`): the TSX type maps to an IR type");
        check_eq(record.type, prop.type,
                 where + " (`" + record.name + "`): the TSX type matches the IR prop type");
        check(record.optional == prop.optional,
              where + " (`" + record.name + "`): `?` in the alias matches the IR optional flag");
        check(record.optional == (prop.default_value != nullptr),
              where + " (`" + record.name +
                  "`): the destructuring default marks exactly the records the alias marks `?`");
    }
}

// The lowered records must be the alias records, in declaration order, with the `?` flag and the
// default the destructuring carries. The source and the generated declaration are compared again
// byte for byte below; these checks name the failing record when they differ.
void check_generated_records(const ir::Component& lowered,
                             const std::vector<DeclaredRecord>& records, std::string_view name) {
    check(lowered.props.size() == records.size(),
          std::string(name) + ": one lowered record per declared alias member, found " +
              std::to_string(lowered.props.size()) + " and " + std::to_string(records.size()));
    const std::size_t count = std::min(records.size(), lowered.props.size());
    for (std::size_t i = 0; i < count; ++i) {
        const std::string where = std::string(name) + " record " + std::to_string(i + 1);
        check_eq(lowered.props[i].name, records[i].name, where + ": name and declaration order");
        check_eq(lowered.props[i].type, records[i].type,
                 where + " (`" + records[i].name + "`): IR type");
        check(lowered.props[i].optional == records[i].optional,
              where + " (`" + records[i].name + "`): the `?` optional flag");
        check((lowered.props[i].default_value != nullptr) == records[i].optional,
              where + " (`" + records[i].name +
                  "`): a destructuring default lowers to the IR record on exactly the `?` props");
    }
}

// The concrete defaults of forms sections 5.1/5.2, pinned by name so a dropped or retyped default
// fails with the record it belongs to rather than only as a JSON diff.
std::string describe_expr(const ir::Expr& expr);

void check_default_literal(const ir::Component& component, std::string_view prop,
                           std::string_view expected, std::string_view name,
                           std::string_view what) {
    const auto found =
        std::find_if(component.props.begin(), component.props.end(),
                     [prop](const ir::Prop& candidate) { return candidate.name == prop; });
    check(found != component.props.end(),
          std::string(name) + " declares `" + std::string(prop) + "`");
    if (found == component.props.end()) {
        return;
    }
    const ir::Expr* value = found->default_value.get();
    check(value != nullptr,
          std::string(name) + " prop `" + std::string(prop) + "` has a default value");
    if (value == nullptr) {
        return;
    }
    const std::string actual = describe_expr(*value);
    check_eq(actual, std::string(expected),
             std::string(name) + " prop `" + std::string(prop) + "` " + std::string(what));
}

std::string describe_expr(const ir::Expr& expr) {
    switch (expr.kind) {
    case ir::ExprKind::Literal:
        if (const auto* text = std::get_if<std::string>(&expr.literal)) {
            return "\"" + *text + "\"";
        }
        if (const auto* flag = std::get_if<bool>(&expr.literal)) {
            return *flag ? "true" : "false";
        }
        if (const auto* whole = std::get_if<std::int64_t>(&expr.literal)) {
            return std::to_string(*whole);
        }
        if (const auto* real = std::get_if<double>(&expr.literal)) {
            return std::to_string(*real);
        }
        return "null";
    case ir::ExprKind::Array:
        return "[" + std::to_string(expr.operands.size()) + " items]";
    default:
        return "expr";
    }
}

void check_defaults(const ir::Component& component, std::string_view name) {
    if (name == "Field") {
        for (const std::string_view prop : {"name", "label", "value", "defaultValue", "error"}) {
            check_default_literal(component, prop, "\"\"", name, "defaults to the empty string");
        }
        check_default_literal(component, "required", "false", name, "defaults to false");
        for (const std::string_view prop : {"validators", "errors"}) {
            check_default_literal(component, prop, "[0 items]", name, "defaults to the empty list");
        }
    } else if (name == "Form") {
        check_default_literal(component, "initial", "[0 items]", name,
                              "defaults to the empty seed list");
    }
    for (const std::string_view prop : {"onValidate", "validate", "onSubmit", "children"}) {
        const auto found =
            std::find_if(component.props.begin(), component.props.end(),
                         [prop](const ir::Prop& candidate) { return candidate.name == prop; });
        if (found != component.props.end()) {
            check(found->default_value == nullptr,
                  std::string(name) + " prop `" + std::string(prop) + "` has no default value");
        }
    }
}

// ---------------------------------------------------------------------------------------------
// The staleness comparison and the per-component driver.

void check_parity(const ir::Component& lowered, const ir::Component& generated,
                  std::string_view name) {
    ir::Component tsx = clone_component(lowered);
    ir::Component expected = clone_component(generated);
    normalize_identity(tsx);
    normalize_identity(expected);
    check_eq(describe_component(tsx), describe_component(expected),
             std::string(name) +
                 ": the committed TSX lowers to exactly the generated IR declaration (identity "
                 "band excluded), including spans, prop defaults and text-run shape");
}

void check_component(std::string_view file, std::string_view name) {
    std::unique_ptr<FileFixture> fixture = parse_source_file(file);
    if (!fixture) {
        check(false, "the library component source is readable: " + std::string(file));
        return;
    }
    const auto result = lowering::lower_module(fixture->module_source(), "library");
    check(result.has_value(),
          std::string(name) + " lowers through the ordinary path: " + first_message(result));
    if (!result) {
        return;
    }
    const ir::Component* lowered = find_component(*result, name);
    check(lowered != nullptr, std::string(name) + " is a component of the lowered module");
    const lowering::LibraryComponent* library = lowering::find_library_component(name);
    check(library != nullptr, std::string(name) + " is a declared library component");
    if (lowered == nullptr || library == nullptr) {
        return;
    }
    const ir::Component generated = lowering::make_library_component(*library);

    check_eq(lowered->id, generated.id, std::string(name) + ": same IR component id");
    check_eq(lowered->name, generated.name, std::string(name) + ": same component name");
    check_eq(lowered->source.file, std::string(file),
             std::string(name) + ": the source span names the real TSX file");
    check(is_presentational(*lowered), std::string(name) + ": the source is presentational");
    check(is_presentational(generated), std::string(name) + ": the generated IR is presentational");

    const jsx::ModuleItem* item = find_item(*fixture->parsed.root, jsx::ItemKind::Component, name);
    check(item != nullptr, std::string(name) + " is a parsed component declaration");
    if (item != nullptr) {
        const std::vector<DeclaredRecord> records = declared_records(*fixture->parsed.root, *item);
        check_generated_records(*lowered, records, name);
        check_declared_records(*fixture->parsed.root, *item, generated, name);
    }
    check_defaults(generated, name);
    check_parity(*lowered, generated, name);
}

} // namespace

int main() {
    check_component(kFieldFile, "Field");
    check_component(kFormFile, "Form");
    return jsx_test::finish("library TSX parity tests");
}
