// SPDX-License-Identifier: MIT

// Public lookups and enum names of the schema library. The tables themselves live in the data
// files; this file only searches them.

#include "tables.hpp"

#include <algorithm>
#include <string_view>

namespace tessera::schema {
namespace {

template<typename Row> const Row* find_by_name(std::span<const Row> rows_, std::string_view name) {
    const auto found =
        std::find_if(rows_.begin(), rows_.end(), [&](const Row& row) { return row.name == name; });
    return found == rows_.end() ? nullptr : &*found;
}

} // namespace

std::string_view to_string(ChildrenKind kind) {
    switch (kind) {
    case ChildrenKind::None:
        return "none";
    case ChildrenKind::Text:
        return "text";
    case ChildrenKind::InlineText:
        return "inline-text";
    case ChildrenKind::Nodes:
        return "nodes";
    case ChildrenKind::TextAndNodes:
        return "text+nodes";
    case ChildrenKind::Template:
        return "template";
    case ChildrenKind::One:
        return "one";
    }
    return "none";
}

std::string_view to_string(ValueKind kind) {
    switch (kind) {
    case ValueKind::Literal:
        return "literal";
    case ValueKind::Expression:
        return "expr";
    case ValueKind::Handler:
        return "handler";
    case ValueKind::Element:
        return "element";
    case ValueKind::Name:
        return "name";
    }
    return "expr";
}

std::string_view to_string(ConstraintKind kind) {
    switch (kind) {
    case ConstraintKind::Requires:
        return "requires";
    case ConstraintKind::Exclusive:
        return "exclusive";
    case ConstraintKind::OneOf:
        return "oneOf";
    case ConstraintKind::ContentOneOf:
        return "contentOneOf";
    case ConstraintKind::OnlyOn:
        return "onlyOn";
    case ConstraintKind::WarnIfMissing:
        return "warnIfMissing";
    case ConstraintKind::Custom:
        return "custom";
    }
    return "custom";
}

std::string_view to_string(AliasStatus status) {
    switch (status) {
    case AliasStatus::Mapped:
        return "mapped";
    case AliasStatus::Partial:
        return "partial";
    case AliasStatus::Unsupported:
        return "unsupported";
    }
    return "unsupported";
}

std::string_view to_string(AliasAttrKind kind) {
    switch (kind) {
    case AliasAttrKind::Keep:
        return "keep";
    case AliasAttrKind::Rename:
        return "rename";
    case AliasAttrKind::Reject:
        return "reject";
    case AliasAttrKind::ToStyle:
        return "toStyle";
    case AliasAttrKind::ValueMap:
        return "valueMap";
    }
    return "keep";
}

std::string_view to_string(StyleKind kind) {
    switch (kind) {
    case StyleKind::Length:
        return "length";
    case StyleKind::Number:
        return "number";
    case StyleKind::Color:
        return "color";
    case StyleKind::Keyword:
        return "keyword";
    case StyleKind::List:
        return "list";
    }
    return "keyword";
}

std::span<const PropRow> common_props() {
    return internal::common_prop_table();
}

std::span<const AliasAttr> common_alias_attrs() {
    return internal::common_alias_attr_table();
}

std::span<const std::string_view> role_tokens() {
    return internal::role_token_table();
}

std::span<const TypeRecord> type_records() {
    return internal::type_record_table();
}

std::span<const ElementRow> elements() {
    return internal::element_table();
}

std::span<const AliasRow> aliases() {
    return internal::alias_table();
}

std::span<const AliasStyle> alias_style_defaults(std::string_view tag) {
    const std::span<const AliasStyle> table = internal::alias_style_table();
    const auto first = std::find_if(table.begin(), table.end(),
                                    [&](const AliasStyle& row) { return row.tag == tag; });
    if (first == table.end()) {
        return {};
    }
    const auto last =
        std::find_if(first, table.end(), [&](const AliasStyle& row) { return row.tag != tag; });
    return std::span<const AliasStyle>(first, last);
}

std::span<const DomType> dom_types() {
    return internal::dom_type_table();
}

std::span<const StyleProperty> style_properties() {
    return internal::style_property_table();
}

const ElementRow* find_element(std::string_view tag) {
    const std::span<const ElementRow> table = internal::element_table();
    const auto found = std::find_if(table.begin(), table.end(),
                                    [&](const ElementRow& row) { return row.tag == tag; });
    return found == table.end() ? nullptr : &*found;
}

const TypeRecord* find_type_record(std::string_view name) {
    return find_by_name(internal::type_record_table(), name);
}

const AliasRow* find_alias(std::string_view tag) {
    const std::span<const AliasRow> table = internal::alias_table();
    const auto found = std::find_if(table.begin(), table.end(),
                                    [&](const AliasRow& row) { return row.tag == tag; });
    return found == table.end() ? nullptr : &*found;
}

const DomType* find_dom_type(std::string_view name) {
    return find_by_name(internal::dom_type_table(), name);
}

const StyleProperty* find_style_property(std::string_view name) {
    return find_by_name(internal::style_property_table(), name);
}

} // namespace tessera::schema
