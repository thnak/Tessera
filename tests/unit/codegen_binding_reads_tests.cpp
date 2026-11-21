// SPDX-License-Identifier: MIT

// ADR-056 slice 1 (issue #441): the binding read-set analysis. Hand-built IR, one input at a time:
// for each binding kind the read set is exactly the cells the expression names, a literal reads
// nothing, and anything the analysis cannot see through reads every cell. A last test checks that
// every dynamic binding the generator collects for the committed fixtures has an analysis entry, so
// slice 2 cannot silently skip one.

#include "binding_reads.hpp"
#include "codegen_dynamic_module.hpp"
#include "codegen_dynamic_virtual_module.hpp"
#include "codegen_sample_module.hpp"
#include "codegen_services_module.hpp"
#include "codegen_virtual_module.hpp"
#include "emit_dynamic.hpp"
#include "interpolation.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace ir = tessera::compiler::ir;
namespace internal = tessera::compiler::codegen::internal;
using internal::BindingKind;
using internal::ReadSet;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

using Cells = std::vector<std::string>;

// How many collected bindings the fixture coverage actually looked at, so that an empty collector
// (a fixture that lost its dynamic parts) cannot pass the coverage test vacuously.
struct Seen {
    int texts = 0;
    int metadata = 0;
    int conditions = 0;
    int lists = 0;
};
Seen seen;

bool reads_exactly(const ReadSet* set, const Cells& cells) {
    return set != nullptr && !set->all && set->cells == cells;
}

ir::Node node(ir::NodeKind kind) {
    ir::Node n;
    n.kind = kind;
    return n;
}

ir::ExprPtr ref(const char* id) {
    return ir::make_ref(id);
}

ir::ExprPtr text_of(std::vector<ir::ExprPtr> refs) {
    std::vector<ir::TemplatePart> parts;
    for (ir::ExprPtr& r : refs) {
        parts.push_back(ir::text_part(" "));
        parts.push_back(ir::expr_part(std::move(r)));
    }
    return ir::make_template(std::move(parts));
}

// A component that declares one cell of every namespace, with `root` already holding `child`s.
ir::Component component_with(ir::Node root) {
    ir::Component component;
    component.id = "comp::C";
    component.name = "C";
    component.props.push_back({"title", "string", nullptr});
    component.state.push_back({"state::count", "count", "int", ir::make_literal(std::int64_t{0})});
    component.state.push_back({"state::items", "items", "list<string>", ir::make_array({})});
    component.state.push_back({"state::name", "name", "string", ir::make_literal(std::string())});
    component.animated_values.push_back({"anim::fade", "fade", "float", ir::make_literal(1.0)});
    component.environment.push_back({"env::theme", "theme", "theme", "string", false, {}, {}});
    component.root = std::move(root);
    return component;
}

ir::Component with_child(ir::Node child) {
    ir::Node root = node(ir::NodeKind::View);
    root.children.push_back(std::move(child));
    return component_with(std::move(root));
}

// Ordinal 1 is the root, so the first child is ordinal 2.
constexpr unsigned kFirstChild = 2;

void test_text_reads_exactly_what_it_names() {
    ir::Node text = node(ir::NodeKind::Text);
    std::vector<ir::ExprPtr> refs;
    refs.push_back(ref("prop::title"));
    refs.push_back(ref("state::count"));
    text.text = text_of(std::move(refs));
    const ir::Component c = with_child(std::move(text));
    const auto table = internal::analyze_binding_reads(c);
    check(
        reads_exactly(table.find(BindingKind::Text, kFirstChild), {"prop::title", "state::count"}),
        "a template text reads exactly the two cells it names");
}

void test_literal_reads_nothing() {
    ir::Node text = node(ir::NodeKind::Text);
    text.text = ir::make_literal(std::string("Header"));
    const ir::Component c = with_child(std::move(text));
    const auto table = internal::analyze_binding_reads(c);
    check(reads_exactly(table.find(BindingKind::Text, kFirstChild), {}),
          "a literal text is present with an empty read set");
}

void test_metadata_and_style() {
    ir::Node view = node(ir::NodeKind::View);
    view.props["className"] = ref("state::name");
    view.props["aria-label"] = ref("prop::title");
    view.props["role"] = ir::make_literal(std::string("group"));
    view.style["opacity"] = ref("anim::fade");
    view.style["width"] = ir::make_binary("+", ref("env::theme"), ref("state::count"));
    const ir::Component c = with_child(std::move(view));
    const auto table = internal::analyze_binding_reads(c);
    check(
        reads_exactly(table.find(BindingKind::Metadata, kFirstChild, "className"), {"state::name"}),
        "className reads its cell");
    check(reads_exactly(table.find(BindingKind::Metadata, kFirstChild, "aria-label"),
                        {"prop::title"}),
          "an aria attribute reads its cell");
    check(reads_exactly(table.find(BindingKind::Metadata, kFirstChild, "role"), {}),
          "a literal role reads nothing");
    check(reads_exactly(table.find(BindingKind::Style, kFirstChild, "opacity"), {"anim::fade"}),
          "an inline style reads the animated cell it names");
    check(reads_exactly(table.find(BindingKind::Style, kFirstChild, "width"),
                        {"env::theme", "state::count"}),
          "a style expression reads both operands");
}

void test_condition() {
    ir::Node branch = node(ir::NodeKind::Text);
    branch.condition = ir::make_binary(">", ref("state::count"), ir::make_literal(std::int64_t{0}));
    const ir::Component c = with_child(std::move(branch));
    const auto table = internal::analyze_binding_reads(c);
    check(reads_exactly(table.find(BindingKind::Condition, kFirstChild), {"state::count"}),
          "a conditional child reads the cells of its condition");
}

void test_conservative_default() {
    const auto reads_all = [](ir::ExprPtr expr, std::string_view what) {
        ir::Node text = node(ir::NodeKind::Text);
        text.text = std::move(expr);
        const ir::Component c = with_child(std::move(text));
        const auto table = internal::analyze_binding_reads(c);
        const ReadSet* set = table.find(BindingKind::Text, kFirstChild);
        check(set != nullptr && set->all && set->cells == internal::declared_cells(c), what);
    };
    std::vector<ir::ExprPtr> args;
    args.push_back(ref("state::count"));
    reads_all(ir::make_call("mystery", std::move(args)), "an unknown call target reads all");
    reads_all(ref("mystery"), "an unrecognized reference reads all");
    reads_all(ref("state::ghost"), "an undeclared state cell reads all");
    reads_all(ref("prop::ghost"), "an undeclared prop reads all");
}

void test_known_call_reads_its_arguments() {
    std::vector<ir::ExprPtr> args;
    args.push_back(ref("state::count"));
    args.push_back(ref("prop::title"));
    ir::Node text = node(ir::NodeKind::Text);
    text.text = ir::make_call("max", std::move(args));
    const ir::Component c = with_child(std::move(text));
    const auto table = internal::analyze_binding_reads(c);
    check(
        reads_exactly(table.find(BindingKind::Text, kFirstChild), {"prop::title", "state::count"}),
        "a pure known call reads what its arguments read");
}

void test_record_field_reads_its_base_cell() {
    ir::Component c = with_child(node(ir::NodeKind::Text));
    c.root.children[0].text = ref("state::name.first");
    const auto table = internal::analyze_binding_reads(c);
    check(reads_exactly(table.find(BindingKind::Text, kFirstChild), {"state::name"}),
          "a dotted field reference reads its base cell");
}

void test_list_template() {
    ir::Node row = node(ir::NodeKind::Text);
    ir::Loop loop;
    loop.item = "row";
    loop.index = "i";
    loop.source = ref("state::items");
    row.loop = std::move(loop);
    row.key = ref("row");
    std::vector<ir::ExprPtr> parts;
    parts.push_back(ref("row"));
    parts.push_back(ref("i"));
    parts.push_back(ref("prop::title"));
    row.text = text_of(std::move(parts));
    const ir::Component c = with_child(std::move(row));
    const auto table = internal::analyze_binding_reads(c);
    check(reads_exactly(table.find(BindingKind::ListSource, kFirstChild), {"state::items"}),
          "the list source reads the list cell");
    check(reads_exactly(table.find(BindingKind::ListItem, kFirstChild),
                        {"prop::title", "state::items"}),
          "per-item content reads the list cell plus what the template names, never the locals");
    check(table.find(BindingKind::Text, kFirstChild) == nullptr,
          "template text is not a component-level binding");
}

void test_virtual_collection_item_template() {
    ir::Node collection = node(ir::NodeKind::VirtualList);
    ir::Node item = node(ir::NodeKind::Text);
    ir::Loop loop;
    loop.item = "row";
    loop.source = ref("state::items");
    item.loop = std::move(loop);
    item.text = text_of([] {
        std::vector<ir::ExprPtr> v;
        v.push_back(ref("row"));
        v.push_back(ref("state::count"));
        return v;
    }());
    collection.item_template = std::make_unique<ir::Node>(std::move(item));
    const ir::Component c = with_child(std::move(collection));
    const auto table = internal::analyze_binding_reads(c);
    // root = 1, collection = 2, item template = 3.
    check(reads_exactly(table.find(BindingKind::ListItem, 3), {"state::count", "state::items"}),
          "a virtual collection's item template is a list template at its own ordinal");
}

void test_controlled_input_value() {
    ir::Node input = node(ir::NodeKind::TextInput);
    input.props["value"] = ref("state::name");
    input.props["placeholder"] = ref("prop::title");
    const ir::Component c = with_child(std::move(input));
    const auto table = internal::analyze_binding_reads(c);
    check(reads_exactly(table.find(BindingKind::Text, kFirstChild), {"state::name"}),
          "a controlled input's value is its Text binding");
    check(table.find(BindingKind::Attribute, kFirstChild, "value") == nullptr,
          "the value prop is not also an attribute");
    check(reads_exactly(table.find(BindingKind::Attribute, kFirstChild, "placeholder"),
                        {"prop::title"}),
          "another prop is an attribute binding");
}

void test_instance_props() {
    ir::Node instance = node(ir::NodeKind::Component);
    instance.component = "comp::Child";
    instance.props["label"] = ref("state::name");
    instance.props["size"] = ir::make_literal(std::int64_t{3});
    const ir::Component c = with_child(std::move(instance));
    const auto table = internal::analyze_binding_reads(c);
    check(
        reads_exactly(table.find(BindingKind::InstanceProp, kFirstChild, "label"), {"state::name"}),
        "an instance prop reads the cell it passes");
    check(reads_exactly(table.find(BindingKind::InstanceProp, kFirstChild, "size"), {}),
          "a literal instance prop reads nothing");
}

void test_effect_inputs() {
    ir::Component c = with_child(node(ir::NodeKind::Text));
    c.effects.push_back({"effect::sync", {"state::count", "prop::title"}, "bind::h", {}, {}});
    c.effects.push_back({"effect::bad", {"state::ghost"}, "bind::h2", {}, {}});
    const auto table = internal::analyze_binding_reads(c);
    check(reads_exactly(table.find(BindingKind::Effect, 0, "effect::sync"),
                        {"prop::title", "state::count"}),
          "an effect reads its deps");
    const ReadSet* bad = table.find(BindingKind::Effect, 1, "effect::bad");
    check(bad != nullptr && bad->all, "an effect dep that is not a declared cell reads all");
}

void test_table_is_sorted_and_deterministic() {
    ir::Node view = node(ir::NodeKind::View);
    view.props["id"] = ref("state::name");
    view.style["width"] = ref("state::count");
    ir::Node text = node(ir::NodeKind::Text);
    text.text = ref("prop::title");
    view.children.push_back(std::move(text));
    const ir::Component c = with_child(std::move(view));
    const auto first = internal::analyze_binding_reads(c);
    const auto second = internal::analyze_binding_reads(c);
    bool sorted = true;
    for (std::size_t i = 1; i < first.entries.size(); ++i) {
        sorted = sorted && first.entries[i - 1].key < first.entries[i].key;
    }
    check(sorted, "entries are sorted by key with no duplicates");
    check(first.entries.size() == second.entries.size(), "two runs agree on the entry count");
    for (std::size_t i = 0; i < first.entries.size() && i < second.entries.size(); ++i) {
        check(first.entries[i].key == second.entries[i].key &&
                  first.entries[i].reads == second.entries[i].reads,
              "two runs produce identical entries");
    }
    check(first.find(BindingKind::Text, 99) == nullptr, "a missing binding is not found");
}

// Every dynamic binding the generator collects for a committed fixture has an analysis entry.
void check_component_coverage(const ir::Component& component, std::string_view label) {
    const auto table = internal::analyze_binding_reads(component);
    const std::string where = std::string(label) + "/" + component.name + ": ";
    std::string error;

    unsigned ordinal = 0;
    std::vector<internal::DynamicText> texts;
    check(internal::collect_dynamic_text(component.root, component, ordinal, texts, error),
          where + "collect_dynamic_text succeeds");
    for (const internal::DynamicText& entry : texts) {
        ++seen.texts;
        check(table.find(BindingKind::Text, entry.ordinal) != nullptr,
              where + "dynamic text at ordinal " + std::to_string(entry.ordinal) + " has an entry");
    }

    ordinal = 0;
    std::vector<internal::DynamicMetadata> metadata;
    check(internal::collect_dynamic_metadata(component.root, component, ordinal, metadata, error),
          where + "collect_dynamic_metadata succeeds");
    for (const internal::DynamicMetadata& entry : metadata) {
        ++seen.metadata;
        std::string name = entry.name;
        BindingKind kind = BindingKind::Metadata;
        switch (entry.field) {
        case internal::MetadataField::ElementId:
            name = "id";
            break;
        case internal::MetadataField::ClassName:
            name = "className";
            break;
        case internal::MetadataField::Role:
            name = "role";
            break;
        case internal::MetadataField::Aria:
            break;
        case internal::MetadataField::Style:
            kind = BindingKind::Style;
            break;
        }
        check(table.find(kind, entry.ordinal, name) != nullptr,
              where + "dynamic metadata `" + name + "` at ordinal " +
                  std::to_string(entry.ordinal) + " has an entry");
    }

    internal::DynamicPlan plan;
    check(internal::collect_dynamic_plan(component, plan, error),
          where + "collect_dynamic_plan succeeds");
    for (const internal::DynamicGroup& group : plan.groups) {
        for (const internal::DynamicChild& child : group.children) {
            if (child.kind == internal::DynamicChildKind::Conditional) {
                ++seen.conditions;
                check(table.find(BindingKind::Condition, child.ordinal) != nullptr,
                      where + "conditional child at ordinal " + std::to_string(child.ordinal) +
                          " has an entry");
            }
        }
    }
    for (const internal::ListTemplate& list : plan.lists) {
        ++seen.lists;
        check(table.find(BindingKind::ListSource, list.ordinal) != nullptr,
              where + "list source at ordinal " + std::to_string(list.ordinal) + " has an entry");
        check(table.find(BindingKind::ListItem, list.ordinal) != nullptr,
              where + "list item content at ordinal " + std::to_string(list.ordinal) +
                  " has an entry");
    }
    for (std::size_t index = 0; index < component.effects.size(); ++index) {
        check(table.find(BindingKind::Effect, static_cast<unsigned>(index),
                         component.effects[index].id) != nullptr,
              where + "effect " + component.effects[index].id + " has an entry");
    }
}

void test_fixture_coverage() {
    const auto run = [](const ir::IrModule& module, std::string_view label) {
        check(!module.components.empty(), std::string(label) + " fixture has components");
        for (const ir::Component& component : module.components) {
            check_component_coverage(component, label);
        }
    };
    run(tessera::test::codegen_dynamic::make_dynamic_module(), "dynamic");
    run(tessera::test::codegen_dynamic_virtual::make_dynamic_virtual_module(), "dynamic_virtual");
    run(tessera::test::codegen_sample::make_sample_module(), "sample");
    run(tessera::test::codegen_virtual::make_virtual_module(), "virtual");
    run(tessera::test::codegen_services::make_services_module(), "services");
    run(tessera::test::codegen_services::make_mixed_module(), "mixed");
    check(seen.texts > 0 && seen.metadata > 0 && seen.conditions > 0 && seen.lists > 0,
          "the fixtures exercise dynamic text, metadata, conditionals and lists");
}

} // namespace

int main() {
    test_text_reads_exactly_what_it_names();
    test_literal_reads_nothing();
    test_metadata_and_style();
    test_condition();
    test_conservative_default();
    test_known_call_reads_its_arguments();
    test_record_field_reads_its_base_cell();
    test_list_template();
    test_virtual_collection_item_template();
    test_controlled_input_value();
    test_instance_props();
    test_effect_inputs();
    test_table_is_sorted_and_deterministic();
    test_fixture_coverage();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen_binding_reads: ok\n";
    return 0;
}
