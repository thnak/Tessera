// SPDX-License-Identifier: MIT

// Slice 4 (partial): a component's declared prop records are lowered from its props type alias (or
// an inline props object type) into `Component::props` (docs/specs/ir-v1.md section 2,
// docs/design/jsx-ir-lowering.md section 9). Every member keeps its declaration order, maps to an
// IR v1 type, and carries the source `?` as the optional flag; the `children` member stays the
// static slot. A member type the V1 list cannot represent is a loud `TESSERA_IR_UNSUPPORTED`.
//
// Determinism: the same source lowers to byte-identical canonical JSON, so a golden or the merger
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

} // namespace

int main() {
    test_alias_records();
    test_inline_object_type();
    test_without_a_resolvable_alias_keeps_the_slot_only();
    test_unsupported_type_is_rejected();
    test_invalid_children_record_is_rejected();
    test_canonical_ir_is_deterministic();
    return jsx_test::finish("lowering props tests");
}
