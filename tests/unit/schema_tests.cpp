// SPDX-License-Identifier: MIT

// Integrity and content checks of the shared schema table (docs/specs/jsx-intrinsics-v1): unique
// names, referential integrity between elements/props/events/types/aliases, the closed vocabularies
// and a few rows of every spec table.

#include "tessera/schema/intrinsics.hpp"
#include "tessera/schema/style.hpp"

#include <algorithm>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace schema = tessera::schema;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void check_eq(std::string_view actual, std::string_view expected, std::string_view message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << "\n  expected: " << expected
                  << "\n  actual:   " << actual << "\n";
        ++failures;
    }
}

const schema::PropRow* find_prop(std::span<const schema::PropRow> props, std::string_view name) {
    for (const schema::PropRow& prop : props) {
        if (prop.name == name) {
            return &prop;
        }
    }
    return nullptr;
}

const schema::EventRow* find_event(const schema::ElementRow& element, std::string_view name) {
    for (const schema::EventRow& event : element.events) {
        if (event.name == name) {
            return &event;
        }
    }
    return nullptr;
}

const schema::AliasAttr* find_attr(const schema::AliasRow& alias, std::string_view name) {
    for (const schema::AliasAttr& attr : alias.attrs) {
        if (attr.name == name) {
            return &attr;
        }
    }
    return nullptr;
}

void test_schema_version_and_counts() {
    check_eq(std::to_string(schema::kSchemaVersion), "1", "the schema version");
    check(schema::elements().size() == 13, "13 intrinsic elements (README section 3)");
    check(schema::type_records().size() == 12,
          "12 payload records (common-and-events.md section 2)");
    check(schema::aliases().size() == 101, "27 mapped/partial and 74 unmapped aliases");
    check(schema::dom_types().size() == 14, "14 DOM types (html-aliases.md section 4)");
    check(schema::style_properties().size() == 73, "73 style longhands");
    check(schema::role_tokens().size() == 22, "22 role tokens (common-and-events.md 1.1)");
    check(schema::common_props().size() == 20, "20 common attributes");
}

void test_element_rows() {
    constexpr std::string_view kExpectedTags[] = {
        "View",   "Text",   "Button",      "TextInput",   "TextArea",      "Image",         "Path",
        "Scroll", "Spacer", "VirtualList", "VirtualGrid", "ErrorBoundary", "CustomElement",
    };
    check(schema::elements().size() == std::size(kExpectedTags), "one row per element");
    for (std::size_t i = 0; i < schema::elements().size() && i < std::size(kExpectedTags); ++i) {
        check_eq(schema::elements()[i].tag, kExpectedTags[i],
                 "the element order of the README table");
    }
    for (const schema::ElementRow& element : schema::elements()) {
        check(schema::find_element(element.tag) == &element, "find_element returns the row");
        check_eq(element.ir_kind, element.tag, "irKind equals the tag");
        for (const schema::PropRow& prop : element.props) {
            check(!prop.name.empty(), "a prop has a name");
            check(!prop.lowers_to.empty(), "a prop names its lowering target");
            check(prop.lowers_to.rfind("props.", 0) == 0 ||
                      prop.lowers_to.rfind("style.", 0) == 0 || prop.lowers_to == "key" ||
                      prop.lowers_to == "refs" || prop.lowers_to.rfind("events.", 0) == 0 ||
                      prop.lowers_to == "automationId" || prop.lowers_to == "children" ||
                      prop.lowers_to.rfind("node.", 0) == 0,
                  "the lowersTo vocabulary of machine-readable.md section 3");
            for (const std::string_view tag : prop.only_on) {
                check(schema::find_element(tag) != nullptr, "onlyOn names an intrinsic");
            }
            for (const std::string_view tag : prop.not_on) {
                check(schema::find_element(tag) != nullptr, "notOn names an intrinsic");
            }
        }
        for (const schema::EventRow& event : element.events) {
            if (!event.payload.empty()) {
                check(schema::find_type_record(event.payload) != nullptr,
                      "an event payload type exists");
            }
        }
        for (const schema::ConstraintRow& constraint : element.constraints) {
            check(!constraint.code.empty(), "a constraint names its diagnostic code");
            for (const std::string_view prop : constraint.props) {
                const bool declared = find_prop(element.props, prop) != nullptr ||
                                      find_event(element, prop) != nullptr ||
                                      find_prop(schema::common_props(), prop) != nullptr;
                check(declared, "a constraint names a declared prop or event");
            }
        }
    }
}

void test_element_details() {
    const schema::ElementRow* view = schema::find_element("View");
    const schema::ElementRow* text = schema::find_element("Text");
    const schema::ElementRow* button = schema::find_element("Button");
    const schema::ElementRow* input = schema::find_element("TextInput");
    const schema::ElementRow* area = schema::find_element("TextArea");
    const schema::ElementRow* image = schema::find_element("Image");
    const schema::ElementRow* spacer = schema::find_element("Spacer");
    const schema::ElementRow* boundary = schema::find_element("ErrorBoundary");
    const schema::ElementRow* custom = schema::find_element("CustomElement");
    check(view != nullptr && text != nullptr && button != nullptr && input != nullptr &&
              area != nullptr && image != nullptr && spacer != nullptr && boundary != nullptr &&
              custom != nullptr,
          "the elements used by the checks exist");
    if (view == nullptr || text == nullptr || button == nullptr || input == nullptr ||
        area == nullptr || image == nullptr || spacer == nullptr || boundary == nullptr ||
        custom == nullptr) {
        return;
    }
    check(view->children == schema::ChildrenKind::TextAndNodes, "View children");
    check(text->children == schema::ChildrenKind::Text, "Text children");
    check(button->children == schema::ChildrenKind::TextAndNodes, "Button children");
    check(input->children == schema::ChildrenKind::None, "TextInput children");
    check(image->children == schema::ChildrenKind::None, "Image children");
    check(spacer->ref_type.empty(), "Spacer rejects ref");
    check(boundary->ref_type.empty(), "ErrorBoundary rejects ref");
    check(!spacer->accepts_class_name && !spacer->accepts_style,
          "Spacer accepts no className/style");
    check(!boundary->accepts_class_name && !boundary->accepts_style,
          "ErrorBoundary accepts no className/style");
    check_eq(input->implied_role, "textbox", "TextInput implied role");
    check_eq(text->implied_role, "text", "Text implied role");
    check_eq(schema::to_string(schema::find_element("VirtualList")->children), "template",
             "VirtualList children");
    check_eq(schema::to_string(boundary->children), "one", "ErrorBoundary children");

    const schema::PropRow* label = find_prop(button->props, "label");
    check(label != nullptr && label->lowers_to == "props.label",
          "Button label lowers to props.label");
    const schema::PropRow* secure = find_prop(input->props, "secure");
    check(secure != nullptr && secure->only_on.size() == 1 && secure->only_on[0] == "TextInput",
          "secure is restricted to TextInput");
    const schema::PropRow* rows = find_prop(area->props, "rows");
    check(rows != nullptr && rows->only_on.size() == 1 && rows->only_on[0] == "TextArea",
          "rows is restricted to TextArea");
    const schema::PropRow* fallback = find_prop(boundary->props, "fallback");
    check(fallback != nullptr && fallback->value == schema::ValueKind::Element &&
              fallback->required && fallback->lowers_to == "node.fallback",
          "fallback is a required element value lowering to node.fallback");
    const schema::PropRow* element = find_prop(custom->props, "element");
    check(element != nullptr && element->required && element->lowers_to == "node.element",
          "CustomElement element");
    check(find_event(*input, "onChange") != nullptr &&
              find_event(*input, "onChange")->payload == "TextChangeEvent",
          "onChange payload");
    check(find_event(*boundary, "onError") != nullptr, "ErrorBoundary onError is an event");
    check(find_event(*custom, "onPointer") != nullptr, "CustomElement onPointer");
    check(schema::find_element("TextInput")->events.size() == 11, "TextInput event rows");
    check(schema::find_element("Image")->events.size() == 1 &&
              find_event(*schema::find_element("Image"), "onSizeChange") != nullptr,
          "Image observes size only");
    check(find_event(*schema::find_element("Spacer"), "onSizeChange") == nullptr &&
              find_event(*boundary, "onSizeChange") == nullptr,
          "box-less kinds reject onSizeChange");

    bool saw_conflict = false;
    for (const schema::ConstraintRow& constraint : button->constraints) {
        if (constraint.kind == schema::ConstraintKind::ContentOneOf) {
            saw_conflict =
                constraint.or_children && constraint.code == "TESSERA_JSX_BUTTON_CONTENT_CONFLICT";
        }
    }
    check(saw_conflict, "Button contentOneOf constraint");
    bool saw_value_both = false;
    for (const schema::ConstraintRow& constraint : input->constraints) {
        if (constraint.kind == schema::ConstraintKind::Exclusive) {
            saw_value_both = constraint.code == "TESSERA_JSX_INPUT_VALUE_BOTH";
        }
    }
    check(saw_value_both, "TextInput value/defaultValue exclusive constraint");
}

void test_common_props_and_roles() {
    const schema::PropRow* key = find_prop(schema::common_props(), "key");
    check(key != nullptr && key->lowers_to == "key" && key->value == schema::ValueKind::Expression,
          "key lowers to the node key");
    const schema::PropRow* ref = find_prop(schema::common_props(), "ref");
    check(ref != nullptr && ref->value == schema::ValueKind::Name && ref->lowers_to == "refs",
          "ref is a name lowering to refs");
    check(ref != nullptr && ref->not_on.size() == 2, "ref is rejected on Spacer and ErrorBoundary");
    const schema::PropRow* automation = find_prop(schema::common_props(), "automationId");
    check(automation != nullptr && automation->lowers_to == "automationId",
          "automationId lowers to the node automationId");
    const schema::PropRow* aria_checked = find_prop(schema::common_props(), "aria-checked");
    check(aria_checked != nullptr && aria_checked->lowers_to == "props.aria-checked",
          "aria-checked keeps its hyphens");
    for (const std::string_view token : schema::role_tokens()) {
        check(!token.empty(), "a role token is not empty");
    }
    check(std::find(schema::role_tokens().begin(), schema::role_tokens().end(), "button") !=
              schema::role_tokens().end(),
          "the role token list includes button");
}

void test_aliases() {
    const schema::AliasRow* div = schema::find_alias("div");
    check(div != nullptr && div->status == schema::AliasStatus::Mapped && div->target == "View",
          "div maps to View");
    const schema::AliasRow* ul = schema::find_alias("ul");
    check(ul != nullptr && ul->inject.size() == 1 && ul->inject[0].name == "role" &&
              ul->inject[0].value == "list",
          "ul injects role=list");
    const schema::AliasRow* h1 = schema::find_alias("h1");
    check(h1 != nullptr && h1->inject.size() == 1 && h1->inject[0].value == "heading",
          "h1 injects role=heading");
    const schema::AliasRow* anchor = schema::find_alias("a");
    check(anchor != nullptr && anchor->status == schema::AliasStatus::Partial &&
              anchor->target == "Text",
          "a is a partial alias of Text");
    const schema::AliasRow* input = schema::find_alias("input");
    check(input != nullptr && input->type_map.size() == 6 && input->type_map_closed,
          "input has a closed type map");
    bool password_secure = false;
    for (const schema::AliasTypeMap& entry : input->type_map) {
        if (entry.type == "password" && entry.inject.size() == 1 &&
            entry.inject[0].name == "secure" && entry.inject[0].value == "true") {
            password_secure = true;
        }
    }
    check(password_secure, "input type=password injects secure");
    const schema::AliasAttr* autocomplete = find_attr(*input, "autoComplete");
    check(autocomplete != nullptr && autocomplete->kind == schema::AliasAttrKind::Rename &&
              autocomplete->rename == "autofill",
          "input autoComplete renames to autofill");
    const schema::AliasRow* textarea = schema::find_alias("textarea");
    const schema::AliasAttr* wrap = textarea != nullptr ? find_attr(*textarea, "wrap") : nullptr;
    check(wrap != nullptr && wrap->values.size() == 3, "textarea wrap value map");
    const schema::AliasRow* img = schema::find_alias("img");
    const schema::AliasAttr* width = img != nullptr ? find_attr(*img, "width") : nullptr;
    check(width != nullptr && width->kind == schema::AliasAttrKind::ToStyle &&
              width->rename == "width",
          "img width becomes style.width");
    const schema::AliasRow* button = schema::find_alias("button");
    const schema::AliasAttr* type = button != nullptr ? find_attr(*button, "type") : nullptr;
    check(type != nullptr && type->kind == schema::AliasAttrKind::ValueMap &&
              type->values.size() == 3,
          "button type value map");
    const schema::AliasRow* table = schema::find_alias("table");
    check(table != nullptr && table->status == schema::AliasStatus::Unsupported &&
              !table->alternative.empty(),
          "table has no mapping and an alternative");
    check(schema::find_alias("nosuchtag") == nullptr, "an unknown lowercase tag is not found");

    for (const schema::AliasRow& alias : schema::aliases()) {
        if (alias.status == schema::AliasStatus::Unsupported) {
            check(alias.target.empty() && !alias.alternative.empty(),
                  "an unsupported alias has no target and an alternative");
            continue;
        }
        const schema::ElementRow* target = schema::find_element(alias.target);
        check(target != nullptr, "a mapped alias targets an intrinsic");
        if (target == nullptr) {
            continue;
        }
        for (const schema::AliasInject& inject : alias.inject) {
            check(find_prop(target->props, inject.name) != nullptr ||
                      find_prop(schema::common_props(), inject.name) != nullptr ||
                      find_event(*target, inject.name) != nullptr,
                  "an injected alias prop exists on the target");
        }
        for (const schema::AliasAttr& attr : alias.attrs) {
            if (attr.kind == schema::AliasAttrKind::Rename) {
                check(find_prop(target->props, attr.rename) != nullptr,
                      "a renamed alias attribute names a target prop");
            }
            if (attr.kind == schema::AliasAttrKind::ToStyle) {
                check(schema::find_style_property(attr.rename) != nullptr,
                      "a toStyle alias attribute names a style property");
            }
            if (attr.kind == schema::AliasAttrKind::ValueMap) {
                check(!attr.values.empty(), "a valueMap attribute has values");
            }
        }
        for (const schema::AliasTypeMap& entry : alias.type_map) {
            for (const schema::AliasInject& inject : entry.inject) {
                check(find_prop(target->props, inject.name) != nullptr ||
                          find_prop(schema::common_props(), inject.name) != nullptr,
                      "a typeMap injects a declared prop");
            }
        }
    }
}

void test_types_and_dom_types() {
    const schema::TypeRecord* change = schema::find_type_record("TextChangeEvent");
    check(change != nullptr && change->primary == "value" && change->fields.size() == 2,
          "TextChangeEvent has the value primary member");
    check(schema::find_type_record("TextTarget") != nullptr, "TextTarget exists");
    check(schema::find_type_record("FocusEvent") != nullptr &&
              schema::find_type_record("FocusEvent")->fields.empty(),
          "FocusEvent has no fields");
    check(schema::find_type_record("nosuch") == nullptr, "an unknown record is not found");
    for (const schema::TypeRecord& record : schema::type_records()) {
        check(!record.name.empty(), "a record has a name");
        for (const schema::TypeField& field : record.fields) {
            check(!field.name.empty() && !field.type.empty(), "a record field is named and typed");
        }
        if (!record.primary.empty()) {
            check(std::any_of(
                      record.fields.begin(), record.fields.end(),
                      [&](const schema::TypeField& field) { return field.name == record.primary; }),
                  "the primary member is a field of the record");
        }
    }
    const schema::DomType* div = schema::find_dom_type("HTMLDivElement");
    check(div != nullptr && div->target == "View", "HTMLDivElement resolves to View");
    check(schema::find_dom_type("HTMLInputElement") != nullptr, "HTMLInputElement exists");
    for (const schema::DomType& type : schema::dom_types()) {
        check(schema::find_element(type.target) != nullptr, "a DOM type targets an intrinsic");
    }
}

void test_style_table() {
    const schema::StyleProperty* width = schema::find_style_property("width");
    check(width != nullptr && width->kind == schema::StyleKind::Length,
          "width is a length property");
    check(schema::find_style_property("opacity")->kind == schema::StyleKind::Number,
          "opacity is a unitless number");
    check(schema::find_style_property("line-height")->kind == schema::StyleKind::Number,
          "line-height is unitless when written as a number");
    check(schema::find_style_property("color")->kind == schema::StyleKind::Color,
          "color is a color");
    check(schema::find_style_property("display")->kind == schema::StyleKind::Keyword,
          "display is a keyword");
    check(schema::find_style_property("font-family")->kind == schema::StyleKind::List,
          "font-family is a list");
    check(schema::find_style_property("border-shadow") == nullptr,
          "an unknown style property is not found");
    for (const schema::StyleProperty& property : schema::style_properties()) {
        check(!property.name.empty(), "a style row has a name");
    }
}

} // namespace

int main() {
    test_schema_version_and_counts();
    test_element_rows();
    test_element_details();
    test_common_props_and_roles();
    test_aliases();
    test_types_and_dom_types();
    test_style_table();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "schema tests passed\n";
    return 0;
}
