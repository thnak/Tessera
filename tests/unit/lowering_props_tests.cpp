// SPDX-License-Identifier: MIT

// Slice 4 (partial): a component's declared prop records are lowered from its props type alias (or
// an inline props object type) into `Component::props` (docs/specs/ir-v1.md section 2,
// docs/design/jsx-ir-lowering.md section 9). Every member keeps its declaration order, maps to an
// IR v1 type, and carries the source `?` as the optional flag; the `children` member stays the
// static slot. A member type the V1 list cannot represent is a loud `TESSERA_IR_UNSUPPORTED`.
//
// Determinism: the same source lowers to byte-identical canonical JSON, so a golden
// never sees a different record order.

#include "lowering_module_test_util.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>

namespace {

using namespace lowering_module_test;

constexpr std::string_view kCard = "import { Children } from \"tessera\";\n"
                                   "type CardProps = {\n"
                                   "  title: string;\n"
                                   "  count?: int;\n"
                                   "  tone: string | null;\n"
                                   "  tags?: string[];\n"
                                   "  required: boolean;\n"
                                   "  onSave: () => void;\n"
                                   "  onChange: TextChangeEvent;\n"
                                   "  children: Children;\n"
                                   "};\n"
                                   "export function Card({ title, children }: CardProps) {\n"
                                   "  return (\n"
                                   "    <View className=\"card\">\n"
                                   "      <Text>{title}</Text>\n"
                                   "      {children}\n"
                                   "    </View>\n"
                                   "  );\n"
                                   "}\n";

struct ExpectedProp {
    const char* name;
    const char* type;
    bool optional;
};

constexpr ExpectedProp kCardRecords[] = {
    {"title", "string", false},
    {"count", "int", true},
    {"tone", "optional<string>", false},
    {"tags", "list<string>", true},
    {"required", "bool", false},
    {"onSave", "callback", false},
    {"onChange", "TextChangeEvent", false},
    {"children", "children", false},
};

const ir::Prop* declared_prop(const ir::Component& component, std::string_view name) {
    for (const ir::Prop& prop : component.props) {
        if (prop.name == name) {
            return &prop;
        }
    }
    return nullptr;
}

std::size_t count_of(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string_view::npos;
         at = text.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

void check_card_records(const ir::Component& card) {
    check(card.props.size() == std::size(kCardRecords),
          "the alias lowers one record per member (found " + std::to_string(card.props.size()) +
              ")");
    const std::size_t count = std::min(card.props.size(), std::size(kCardRecords));
    for (std::size_t i = 0; i < count; ++i) {
        const ExpectedProp& expected = kCardRecords[i];
        const ir::Prop& prop = card.props[i];
        const std::string where = "record " + std::to_string(i + 1);
        check_eq(prop.name, std::string(expected.name), where + ": declaration order and name");
        check_eq(prop.type, std::string(expected.type), where + " (`" + prop.name + "`): IR type");
        check(prop.optional == expected.optional,
              where + " (`" + prop.name + "`): the `?` optional flag");
    }
    const ir::Prop* children = declared_prop(card, "children");
    check(children != nullptr && children->type == "children" && !children->optional &&
              children->default_value == nullptr,
          "the `children` member stays the static slot record");
    check(count_of(describe_component(card), "\"optional\": true") == 2,
          "only the two `?` members carry the optional flag");
}

void test_alias_records() {
    const auto fixture = parse_file("ui/Card.tsx", std::string(kCard));
    const auto result = lowering::lower_module(fixture->module_source(), "demo");
    check(result.has_value(), "a typed props alias lowers: " + first_message(result));
    if (!result) {
        return;
    }
    const ir::Component* card = find_component(*result, "Card");
    check(card != nullptr, "the Card component is present");
    if (card != nullptr) {
        check_card_records(*card);
        // The slot still works on the node tree.
        const ir::Node& root = card->root;
        check(!root.children.empty() && root.children.back().kind == ir::NodeKind::Slot,
              "`{children}` still lowers to a Slot node");
    }
}

void test_inline_object_type() {
    const auto fixture =
        parse_file("ui/Badge.tsx",
                   "function Badge({ label, children }: { label: string; children: Children }) {\n"
                   "  return <View><Text>{label}</Text>{children}</View>;\n"
                   "}\n");
    const auto result = lowering::lower_module(fixture->module_source(), "demo");
    check(result.has_value(), "an inline props object type lowers: " + first_message(result));
    if (!result) {
        return;
    }
    const ir::Component* badge = find_component(*result, "Badge");
    check(badge != nullptr, "the Badge component is present");
    if (badge == nullptr) {
        return;
    }
    check(badge->props.size() == 2, "the inline type lowers two records");
    if (badge->props.size() == 2) {
        check_eq(badge->props[0].name, "label", "the inline records keep declaration order");
        check_eq(badge->props[0].type, "string", "`label: string` maps to `string`");
        check_eq(badge->props[1].name, "children", "the slot is the second record");
        check_eq(badge->props[1].type, "children", "`children: Children` stays the slot");
    }
}

void test_without_a_resolvable_alias_keeps_the_slot_only() {
    // The pre-slice-4 behavior for a component-level entry point: no module alias is resolvable,
    // so only the destructured `children` slot is recorded (never a silent partial lowering of the
    // remaining members into nothing).
    const auto fixture = parse_file("ui/Legacy.tsx", "function Legacy({ children }: Props) {\n"
                                                     "  return <View>{children}</View>;\n"
                                                     "}\n");
    const auto result = lowering::lower_module(fixture->module_source(), "demo");
    check(result.has_value(),
          "an unresolved props alias is not an error: " + first_message(result));
    if (!result) {
        return;
    }
    const ir::Component* legacy = find_component(*result, "Legacy");
    check(legacy != nullptr, "the Legacy component is present");
    if (legacy != nullptr) {
        check(legacy->props.size() == 1, "only the `children` slot is recorded");
        check(!legacy->props.empty() && legacy->props[0].name == "children",
              "the one record is the slot");
    }
}

void test_unsupported_type_is_rejected() {
    const auto fixture = parse_file("ui/Bad.tsx", "type BadProps = {\n"
                                                  "  title: string;\n"
                                                  "  meta: { flag: boolean };\n"
                                                  "};\n"
                                                  "function Bad({ title }: BadProps) {\n"
                                                  "  return <View><Text>{title}</Text></View>;\n"
                                                  "}\n");
    const auto result = lowering::lower_module(fixture->module_source(), "demo");
    check(!result.has_value(), "an inline value record type has no IR form and is rejected");
    if (result) {
        return;
    }
    check_eq(first_code(result), "TESSERA_IR_UNSUPPORTED", "the documented rejection code");
    const std::string message = first_message(result);
    check(message.find("meta") != std::string::npos, "the diagnostic names the prop: " + message);
    check(message.find("{flag:boolean}") != std::string::npos,
          "the diagnostic names the source type: " + message);
}

void test_invalid_children_record_is_rejected() {
    const auto fixture = parse_file("ui/Slot.tsx", "type SlotProps = { children?: Children };\n"
                                                   "function Slotted({ children }: SlotProps) {\n"
                                                   "  return <View>{children}</View>;\n"
                                                   "}\n");
    const auto result = lowering::lower_module(fixture->module_source(), "demo");
    check(!result.has_value(), "an optional `children` is not a slot declaration");
    if (!result) {
        check_eq(first_code(result), "TESSERA_IR_SLOT", "the slot rejection code");
    }
}

void test_canonical_ir_is_deterministic() {
    const auto first_fixture = parse_file("ui/Card.tsx", std::string(kCard));
    const auto first = lowering::lower_module(first_fixture->module_source(), "demo");
    const auto second_fixture = parse_file("ui/Card.tsx", std::string(kCard));
    const auto second = lowering::lower_module(second_fixture->module_source(), "demo");
    check(first.has_value() && second.has_value(), "the Card fixture lowers twice");
    if (!first || !second) {
        return;
    }
    check_eq(describe(*first), describe(*second),
             "two lowers of the same input are byte-identical");
    // The canonical JSON is also stable across repeated serialization of the same IR.
    const std::string once = describe_component(first->components[0]);
    const std::string twice = describe_component(first->components[0]);
    check_eq(once, twice, "canonical JSON re-serializes identically");
    check(once.find("\"name\": \"title\"") < once.find("\"name\": \"count\""),
          "the records are written in declaration order");
}

// ADR-051 slice 3 (issue #158): an object literal in a declared record's position lowers to an
// `ir::Record`, and the declared record reaches the module type table exactly once. The two
// positions the ADR allows are covered (a prop default and a `useState` initial); the codegen
// consequence lives in `lowering_props_alias_tests.cpp`, which already links the generator.
constexpr std::string_view kRecordSource =
    "type Spacing = {\n"
    "  top: int;\n"
    "  bottom: int;\n"
    "  label?: string;\n"
    "};\n"
    "type BoxProps = {\n"
    "  padding: Spacing;\n"
    "  title: string;\n"
    "  children: Children;\n"
    "};\n"
    "export function Box({ padding = { top: 4, bottom: 4 }, title, children }: BoxProps) {\n"
    "  const [margin, setMargin] = useState<Spacing>({ label: \"m\", bottom: 2, top: 1 });\n"
    "  return <View><Text>{title}</Text>{children}</View>;\n"
    "}\n";

// A literal field value as its text; a non-literal would be a bug in this slice, so it is named.
std::string field_value_text(const ir::Expr& expr) {
    if (expr.kind != ir::ExprKind::Literal) {
        return expr.kind == ir::ExprKind::Record ? "<record>" : "<expression>";
    }
    if (const auto* flag = std::get_if<bool>(&expr.literal)) {
        return *flag ? "true" : "false";
    }
    if (const auto* number = std::get_if<std::int64_t>(&expr.literal)) {
        return std::to_string(*number);
    }
    if (const auto* real = std::get_if<double>(&expr.literal)) {
        return std::to_string(*real);
    }
    if (const auto* text = std::get_if<std::string>(&expr.literal)) {
        return *text;
    }
    return "null";
}

// `Spacing{top=4;bottom=4;}`: the record name, then every field in *member* order.
std::string record_text(const ir::Expr& expr) {
    std::string out = expr.type + "{";
    for (const ir::RecordField& field : expr.fields) {
        out += field.name + "=";
        out += field.value ? field_value_text(*field.value) : std::string("<none>");
        out += ";";
    }
    return out + "}";
}

const ir::RecordType* find_type(const lowering::LoweredModule& module, std::string_view name) {
    for (const ir::RecordType& record : module.types) {
        if (record.name == name) {
            return &record;
        }
    }
    return nullptr;
}

void test_record_default_and_state_are_lowered() {
    Project project;
    project.add(parse_file("ui/Box.tsx", std::string(kRecordSource)));
    auto result = project.lower("demo");
    check(result.has_value(), "the record fixture lowers: " + first_message(result));
    if (!result) {
        return;
    }
    // The table holds the value record once and not the props alias: a props alias is the component
    // signature (`Component::props`), and ADR-051 section 1 forbids its `children`/callback members
    // in a record.
    check_eq(std::to_string(result->types.size()), std::string("1"),
             "only the value record reaches the module type table");
    check(find_type(*result, "BoxProps") == nullptr,
          "the props alias is not a record: it is the component signature");
    const ir::RecordType* spacing = find_type(*result, "Spacing");
    check(spacing != nullptr, "the record type is in the table");
    if (spacing != nullptr) {
        check_eq(spacing->id, std::string("type::Spacing"), "the id is `type::<name>`");
        check(spacing->members.size() == 3, "every member is in the table");
        if (spacing->members.size() == 3) {
            check_eq(spacing->members[0].name + "/" + spacing->members[0].type,
                     std::string("top/int"), "members keep declaration order");
            check(spacing->members[2].optional, "the source `?` is the optional flag");
        }
        check_eq(spacing->source.file, std::string("ui/Box.tsx"), "the record carries its file");
    }

    const ir::Component* box = find_component(*result, "Box");
    check(box != nullptr, "Box lowers");
    if (box == nullptr) {
        return;
    }
    const ir::Prop* padding = nullptr;
    for (const ir::Prop& prop : box->props) {
        if (prop.name == "padding") {
            padding = &prop;
        }
    }
    check(padding != nullptr && padding->default_value != nullptr,
          "the record-typed prop keeps its default value");
    if (padding != nullptr && padding->default_value != nullptr) {
        check(padding->default_value->kind == ir::ExprKind::Record,
              "the prop default is a record value");
        // Fields follow the member order, not the source order, and the omitted optional member is
        // absent rather than empty.
        check_eq(record_text(*padding->default_value), std::string("Spacing{top=4;bottom=4;}"),
                 "the prop default follows member order");
    }
    check(box->state.size() == 1, "the useState cell lowers");
    if (box->state.size() == 1) {
        check_eq(box->state[0].type, std::string("Spacing"), "the state type is the record name");
        check(box->state[0].initial != nullptr &&
                  box->state[0].initial->kind == ir::ExprKind::Record,
              "the state initial is a record value");
        if (box->state[0].initial != nullptr) {
            check_eq(record_text(*box->state[0].initial),
                     std::string("Spacing{top=1;bottom=2;label=m;}"),
                     "the state initial reorders to member order");
        }
    }

    // Round-trip: there is no IR reader (jsx-ir-lowering decision 15), so the check is that the
    // canonical JSON is stable and carries the record shape of ir-v1-expr.md section 4.3.
    ir::IrModule module;
    module.name = result->name;
    module.components = std::move(result->components);
    module.bindings = std::move(result->bindings);
    module.stylesheets = std::move(result->stylesheets);
    module.types = std::move(result->types);
    const std::string json = ir::write_ir_json(module);
    check_eq(ir::write_ir_json(module), json, "the IR JSON is stable across two writes");
    check(json.find("\"kind\": \"record\"") != std::string::npos,
          "the record value is written as a record expression");
    check(json.find("\"type\": \"Spacing\"") != std::string::npos,
          "the record value names its record type");
    check(json.find("\"id\": \"type::Spacing\"") != std::string::npos,
          "the record type carries its `type::` id");
    check(json.find("\"irVersion\": 1") != std::string::npos,
          "the record table is additive: irVersion stays 1");
}

void test_record_rejections_are_loud() {
    struct Case {
        const char* what;
        const char* source;
        const char* code;
        const char* names;
    };
    const Case cases[] = {
        {"a computed field value",
         "type Spacing = { top: int };\n"
         "type BoxProps = { padding: Spacing; children: Children };\n"
         "export function Box({ padding = { top: compute() }, children }: BoxProps) {\n"
         "  return <View>{children}</View>;\n}\n",
         "TESSERA_IR_UNSUPPORTED", "field `top` of record `Spacing`"},
        {"a shorthand identifier value",
         "type Spacing = { top: int };\n"
         "type BoxProps = { padding: Spacing; children: Children };\n"
         "export function Box({ padding = { top: 1, bottom }, children }: BoxProps) {\n"
         "  return <View>{children}</View>;\n}\n",
         "TESSERA_IR_RECORD_VALUE", "no member `bottom`"},
        {"a missing required member",
         "type Spacing = { top: int; bottom: int };\n"
         "type BoxProps = { padding: Spacing; children: Children };\n"
         "export function Box({ padding = { top: 1 }, children }: BoxProps) {\n"
         "  return <View>{children}</View>;\n}\n",
         "TESSERA_IR_RECORD_VALUE", "missing the required field `bottom`"},
        {"an unknown member",
         "type Spacing = { top: int };\n"
         "type BoxProps = { padding: Spacing; children: Children };\n"
         "export function Box({ padding = { top: 1, left: 2 }, children }: BoxProps) {\n"
         "  return <View>{children}</View>;\n}\n",
         "TESSERA_IR_RECORD_VALUE", "no member `left`"},
        {"a repeated member",
         "type Spacing = { top: int };\n"
         "type BoxProps = { padding: Spacing; children: Children };\n"
         "export function Box({ padding = { top: 1, top: 2 }, children }: BoxProps) {\n"
         "  return <View>{children}</View>;\n}\n",
         "TESSERA_IR_RECORD_VALUE", "has the field `top` twice"},
        {"an object literal for a non-record prop",
         "type BoxProps = { padding: string; children: Children };\n"
         "export function Box({ padding = { top: 1 }, children }: BoxProps) {\n"
         "  return <View>{children}</View>;\n}\n",
         "TESSERA_IR_UNSUPPORTED", "is not a record of this module"},
    };
    for (const Case& test : cases) {
        Project project;
        project.add(parse_file("ui/Box.tsx", test.source));
        const auto result = project.lower("demo");
        if (result) {
            check(false, std::string(test.what) + " is rejected");
            continue;
        }
        check_eq(first_code(result), std::string(test.code),
                 std::string(test.what) + " uses " + test.code);
        check(first_message(result).find(test.names) != std::string::npos,
              std::string(test.what) + " names the member: " + first_message(result));
    }
}

} // namespace

int main() {
    test_alias_records();
    test_inline_object_type();
    test_without_a_resolvable_alias_keeps_the_slot_only();
    test_unsupported_type_is_rejected();
    test_invalid_children_record_is_rejected();
    test_canonical_ir_is_deterministic();
    test_record_default_and_state_are_lowered();
    test_record_rejections_are_loud();
    return jsx_test::finish("lowering props tests");
}
