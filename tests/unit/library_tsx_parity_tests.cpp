// SPDX-License-Identifier: MIT

// `Field` and `Form` are authored as real TSX sources (tessera/components/Field.tsx and
// tessera/components/Form.tsx, issue #70). This test compiles them through the ordinary parser,
// analysis and lowering path and pins the result against the hand-built IR declaration of
// `make_library_component()` (src/compiler/lowering/src/library_components.cpp, which this slice
// deliberately does not change). The two must not drift: an edit to a source file, to the lowering
// pipeline, or to the hand-built table fails here.
//
// The comparison is structural: both components are serialized with the canonical
// `ir::to_text(ir::to_json(component))` and compared byte for byte. Three differences are
// normalized first, because the current pipeline cannot reproduce them from source:
//
// 1. Identity. `Component::component_id` and `Node::ui_id` are filled by the identity-assignment
//    slice (10) and are empty after lowering; the hand-built table hardcodes magic ids
//    (900000000000000001, ...). Both are cleared.
// 2. Spans. The hand-built table synthesizes spans that point into no real source text (offset 0
//    with length 400 for the component, 12-byte node spans). Only `SourceSpan::file` is compared,
//    because the file name is exactly what issue #70 is about: the IR names a file that did not
//    exist. Every node in the lowered tree keeps that file name after normalization.
// 3. The text-run shape. Lowering turns a JSX text run into a `template` ("a run is a literal
//    string or a `template`", docs/design/jsx-ir-lowering.md), so `{label}` lowers to a one-part
//    template. The hand-built tree stores the bare `ref` the template wraps. The comparison
//    collapses a one-expression template to its expression; `check_text_run_shape_gap` pins the
//    raw shapes so the divergence stays visible instead of disappearing into the normalization.
//
// Declared prop records are lowered from the TSX props alias (issue #70): every member becomes one
// IR record with its name, mapped IR type and `?` optional flag. The one remaining difference is
// the default value: a type alias has no default syntax, so lowering carries `optional` while the
// hand-built table carries a default on exactly those records. `check_declared_records` pins the
// alias-to-record mapping and `check_default_gap` pins the unlowered defaults; the structural
// comparison folds a default into `optional` and clears it, so the rest of the record must match.

#include "lowering_module_test_util.hpp"
#include "tessera/compiler/lowering/library_components.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef TESSERA_LIBRARY_TSX_DIR
#error "TESSERA_LIBRARY_TSX_DIR must be defined by the build"
#endif

namespace {

using namespace lowering_module_test;
namespace fs = std::filesystem;

constexpr std::string_view kFieldFile = "tessera/components/Field.tsx";
constexpr std::string_view kFormFile = "tessera/components/Form.tsx";

// The records whose default value the pipeline still does not lower: the alias has no default
// syntax, so the IR records `optional` instead. Kept as literals so a changed capability fails
// loudly instead of being absorbed by a predicate.
constexpr std::string_view kFieldDefaultRecords =
    "name,label,value,defaultValue,required,validators,error,errors";
constexpr std::string_view kFormDefaultRecords = "initial";

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
// Deep copies, so normalization never mutates the lowering result or the hand-built declaration.

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

// ---------------------------------------------------------------------------------------------
// Normalization of the three differences listed in the header comment.

void normalize_span(ir::SourceSpan& span) {
    span.offset = 0;
    span.length = 0;
    span.line = 1;
    span.column = 1;
    span.end_line = 1;
    span.end_column = 1;
}

ir::ExprPtr normalize_expr(ir::ExprPtr expr) {
    if (!expr) {
        return nullptr;
    }
    for (ir::TemplatePart& part : expr->parts) {
        if (part.expr) {
            part.expr = normalize_expr(std::move(part.expr));
        }
    }
    for (ir::ExprPtr& operand : expr->operands) {
        operand = normalize_expr(std::move(operand));
    }
    // A run of exactly one expression is the only template the comparison folds. A template with
    // literal text (even empty text around an expression) is a real template and stays.
    if (expr->kind == ir::ExprKind::Template && expr->parts.size() == 1 &&
        expr->parts.front().expr && expr->parts.front().text.empty()) {
        return std::move(expr->parts.front().expr);
    }
    return expr;
}

void normalize_node(ir::Node& node) {
    node.ui_id.clear();
    normalize_span(node.source);
    node.key = normalize_expr(std::move(node.key));
    for (auto& entry : node.props) {
        entry.second = normalize_expr(std::move(entry.second));
    }
    for (auto& entry : node.style) {
        entry.second = normalize_expr(std::move(entry.second));
    }
    node.text = normalize_expr(std::move(node.text));
    node.condition = normalize_expr(std::move(node.condition));
    for (ir::Node& child : node.children) {
        normalize_node(child);
    }
    if (node.loop) {
        node.loop->source = normalize_expr(std::move(node.loop->source));
    }
    if (node.fallback) {
        normalize_node(*node.fallback);
    }
    if (node.item_template) {
        normalize_node(*node.item_template);
    }
}

void normalize_component(ir::Component& component) {
    component.component_id.clear();
    normalize_span(component.source);
    for (ir::Prop& prop : component.props) {
        // Lowering records the source `?` as `optional`; the hand-built table records a default on
        // exactly those props. Fold the default into the flag and drop it (the documented gap), so
        // the rest of the record is compared.
        prop.optional = prop.optional || prop.default_value != nullptr;
        prop.default_value.reset();
    }
    normalize_node(component.root);
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

// The source type -> IR prop type mapping of forms sections 5.1/5.2. It is duplicated here on
// purpose: the type table that will own it is slice 4 and does not exist yet, so this test pins the
// documented mapping instead of assuming it.
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
                            const ir::Component& hand, std::string_view name) {
    const std::vector<DeclaredRecord> records = declared_records(module, component);
    check(records.size() == hand.props.size(),
          std::string(name) + ": the TSX alias declares one record per hand-built prop, found " +
              std::to_string(records.size()) + " and " + std::to_string(hand.props.size()));
    const std::size_t count = std::min(records.size(), hand.props.size());
    for (std::size_t i = 0; i < count; ++i) {
        const std::string where = std::string(name) + " prop " + std::to_string(i + 1);
        const DeclaredRecord& record = records[i];
        const ir::Prop& prop = hand.props[i];
        check_eq(record.name, prop.name, where + ": the TSX record name matches the IR record");
        check(!record.type.empty(),
              where + " (`" + record.name + "`): the TSX type maps to an IR type");
        check_eq(record.type, prop.type,
                 where + " (`" + record.name + "`): the TSX type matches the IR prop type");
        check(record.optional == (prop.default_value != nullptr),
              where + " (`" + record.name +
                  "`): `?` in the alias marks exactly the hand-built records with a default");
    }
}

// The lowered records must be the alias records, in declaration order, with the `?` flag. A default
// value is the one thing lowering does not fill yet (the alias has no default syntax).
void check_lowered_records(const ir::Component& lowered, const std::vector<DeclaredRecord>& records,
                           std::string_view name) {
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
        check(lowered.props[i].default_value == nullptr,
              where + " (`" + records[i].name + "`): a prop default stays a documented gap");
    }
}

std::string join_prop_names_with_defaults(const ir::Component& component) {
    std::string joined;
    for (const ir::Prop& prop : component.props) {
        if (prop.default_value == nullptr) {
            continue;
        }
        if (!joined.empty()) {
            joined.push_back(',');
        }
        joined += prop.name;
    }
    return joined;
}

// The remaining #70 delta: the hand-built table still carries a default value on these records
// while the TSX alias can only carry `?`. Fails if lowering starts filling defaults (then the list
// shrinks) or if the hand-built table gains one the alias cannot express.
void check_default_gap(const ir::Component& hand, std::string_view name,
                       std::string_view expected_records) {
    check_eq(join_prop_names_with_defaults(hand), std::string(expected_records),
             std::string(name) +
                 ": the hand-built records still carrying a default value. The TSX alias records "
                 "the same props as `?` and lowering keeps `default` empty; a default value (and a "
                 "list/object literal default) is the remaining documented #70 gap");
}

// ---------------------------------------------------------------------------------------------
// The text-run shape gap (see the header comment, item 3).

bool is_one_expression_template(const ir::Expr* expr) {
    return expr != nullptr && expr->kind == ir::ExprKind::Template && expr->parts.size() == 1 &&
           expr->parts.front().expr != nullptr && expr->parts.front().text.empty();
}

void check_text_run_shape_gap(const ir::Component& lowered, const ir::Component& hand,
                              std::string_view name) {
    std::size_t text_nodes = 0;
    const std::size_t count = std::min(lowered.root.children.size(), hand.root.children.size());
    for (std::size_t i = 0; i < count; ++i) {
        const ir::Node& tsx_node = lowered.root.children[i];
        const ir::Node& hand_node = hand.root.children[i];
        if (tsx_node.kind != ir::NodeKind::Text) {
            continue;
        }
        ++text_nodes;
        check(is_one_expression_template(tsx_node.text.get()),
              std::string(name) + " text node " + std::to_string(i + 1) +
                  ": the subset lowers a one-expression run to a one-part template");
        check(hand_node.text != nullptr && hand_node.text->kind == ir::ExprKind::Ref,
              std::string(name) + " text node " + std::to_string(i + 1) +
                  ": the hand-built IR stores a bare ref, a shape the JSX front end cannot emit; "
                  "the comparison folds the one-part template so this gap stays visible here");
    }
    if (name == "Field") {
        check(text_nodes == 3, "Field has the label, value and error text nodes");
    }
}

// ---------------------------------------------------------------------------------------------
// The structural comparison and the per-component driver.

void check_parity(const ir::Component& lowered, const ir::Component& hand, std::string_view name) {
    ir::Component tsx = clone_component(lowered);
    normalize_component(tsx);
    ir::Component expected = clone_component(hand);
    normalize_component(expected);
    check_eq(describe_component(tsx), describe_component(expected),
             std::string(name) +
                 ": the TSX lowering and make_library_component() are the same component (records "
                 "included) after the documented identity/span/run-shape/default normalizations");
}

void check_component(std::string_view file, std::string_view name,
                     std::string_view expected_defaults) {
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
    const ir::Component hand = lowering::make_library_component(*library);

    check_eq(lowered->id, hand.id, std::string(name) + ": same IR component id");
    check_eq(lowered->name, hand.name, std::string(name) + ": same component name");
    check_eq(lowered->source.file, std::string(file),
             std::string(name) + ": the source span names the real TSX file");
    check(is_presentational(*lowered), std::string(name) + ": the source is presentational");
    check(is_presentational(hand), std::string(name) + ": the hand-built IR is presentational");

    const jsx::ModuleItem* item = find_item(*fixture->parsed.root, jsx::ItemKind::Component, name);
    check(item != nullptr, std::string(name) + " is a parsed component declaration");
    if (item != nullptr) {
        const std::vector<DeclaredRecord> records = declared_records(*fixture->parsed.root, *item);
        check_lowered_records(*lowered, records, name);
        check_declared_records(*fixture->parsed.root, *item, hand, name);
    }
    check_default_gap(hand, name, expected_defaults);
    check_text_run_shape_gap(*lowered, hand, name);
    check_parity(*lowered, hand, name);
}

} // namespace

int main() {
    check_component(kFieldFile, "Field", kFieldDefaultRecords);
    check_component(kFormFile, "Form", kFormDefaultRecords);
    return jsx_test::finish("library TSX parity tests");
}
