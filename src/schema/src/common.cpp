// SPDX-License-Identifier: MIT

// Common and reserved attributes, role tokens and the attribute rewrites shared by every lowercase
// alias (docs/specs/jsx-intrinsics-v1/common-and-events.md section 1 and html-aliases.md 3.1).

#include "tables.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace tessera::schema::internal {
namespace {

template<typename Row, std::size_t N> constexpr std::span<const Row> rows(const Row (&list)[N]) {
    return std::span<const Row>(list, N);
}

constexpr std::string_view kHiddenExcept[] = {"Spacer", "ErrorBoundary"};

template<std::size_t N>
constexpr std::span<const std::string_view> tags(const std::string_view (&list)[N]) {
    return std::span<const std::string_view>(list, N);
}

constexpr PropRow kCommonProps[] = {
    {"key",
     "string | int",
     ValueKind::Expression,
     false,
     "",
     "key",
     {},
     {},
     "required on the root element of a `.map` callback; elsewhere rejected; not bool or float"},
    {"ref",
     "name of a useRef<T>",
     ValueKind::Name,
     false,
     "",
     "refs",
     {},
     tags(kHiddenExcept),
     "T must equal the element type; callback and object refs are rejected"},
    {"className",
     "string",
     ValueKind::Expression,
     false,
     "",
     "props.className",
     {},
     tags(kHiddenExcept),
     "space-separated class names"},
    {"style",
     "object literal",
     ValueKind::Literal,
     false,
     "",
     "style",
     {},
     tags(kHiddenExcept),
     "keys are literal camelCase; values are literals or expressions"},
    {"id",
     "string",
     ValueKind::Literal,
     false,
     "",
     "props.id",
     {},
     tags(kHiddenExcept),
     "selector target only; not a label target and not an automation id"},
    {"role",
     "enum literal",
     ValueKind::Literal,
     false,
     "",
     "props.role",
     {},
     tags(kHiddenExcept),
     "one of the role tokens; an explicit role replaces the implied role"},
    {"aria-label",
     "string",
     ValueKind::Expression,
     false,
     "",
     "props.aria-label",
     {},
     tags(kHiddenExcept),
     "accessible name; overrides a derived name"},
    {"aria-hidden",
     "bool",
     ValueKind::Expression,
     false,
     "false",
     "props.aria-hidden",
     {},
     tags(kHiddenExcept),
     "leaves the node and its subtree out of the accessibility tree"},
    {"aria-disabled",
     "bool",
     ValueKind::Expression,
     false,
     "false",
     "props.aria-disabled",
     {},
     tags(kHiddenExcept),
     ""},
    {"aria-selected",
     "bool",
     ValueKind::Expression,
     false,
     "false",
     "props.aria-selected",
     {},
     tags(kHiddenExcept),
     ""},
    {"aria-expanded",
     "bool",
     ValueKind::Expression,
     false,
     "false",
     "props.aria-expanded",
     {},
     tags(kHiddenExcept),
     ""},
    {"aria-busy",
     "bool",
     ValueKind::Expression,
     false,
     "false",
     "props.aria-busy",
     {},
     tags(kHiddenExcept),
     ""},
    {"aria-invalid",
     "bool",
     ValueKind::Expression,
     false,
     "false",
     "props.aria-invalid",
     {},
     tags(kHiddenExcept),
     ""},
    {"aria-readonly",
     "bool",
     ValueKind::Expression,
     false,
     "false",
     "props.aria-readonly",
     {},
     tags(kHiddenExcept),
     ""},
    {"aria-required",
     "bool",
     ValueKind::Expression,
     false,
     "false",
     "props.aria-required",
     {},
     tags(kHiddenExcept),
     ""},
    {"aria-checked",
     "\"true\" | \"false\" | \"mixed\"",
     ValueKind::Expression,
     false,
     "",
     "props.aria-checked",
     {},
     tags(kHiddenExcept),
     "checked/mixed state"},
    {"aria-valuenow",
     "int",
     ValueKind::Expression,
     false,
     "",
     "props.aria-valuenow",
     {},
     tags(kHiddenExcept),
     "slider current value"},
    {"aria-valuemin",
     "int",
     ValueKind::Expression,
     false,
     "",
     "props.aria-valuemin",
     {},
     tags(kHiddenExcept),
     "slider lower bound"},
    {"aria-valuemax",
     "int",
     ValueKind::Expression,
     false,
     "",
     "props.aria-valuemax",
     {},
     tags(kHiddenExcept),
     "slider upper bound"},
    {"automationId",
     "string literal, decimal uint64",
     ValueKind::Literal,
     false,
     "",
     "automationId",
     {},
     {},
     "must equal the computed uiId (ir-v1 section 3.1)"},
    {"part",
     "string",
     ValueKind::Literal,
     false,
     "",
     "props.part",
     {},
     tags(kHiddenExcept),
     "space-separated part names that expose the element to its component host "
     "(`::part()`, stylesheet-v1 section 6.3)"},
};

constexpr AliasAttr kCommonAliasAttrs[] = {
    {"class", AliasAttrKind::Reject, "", "write `className`", {}, ""},
    {"tabIndex", AliasAttrKind::Keep, "", "", {}, ""},
    {"accessKey", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"title",
     AliasAttrKind::Reject,
     "",
     "not a tooltip; use the overlay service or `aria-label`",
     {},
     ""},
    {"lang", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"dir", AliasAttrKind::Reject, "", "no V1 meaning; use the `direction` style", {}, ""},
    {"draggable", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"contentEditable", AliasAttrKind::Reject, "", "no V1 meaning; use `TextInput`", {}, ""},
    {"data-*", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"hidden",
     AliasAttrKind::Reject,
     "",
     "use `{cond && <X />}` or `style={{ display: \"none\" }}`",
     {},
     ""},
};

constexpr std::string_view kRoleTokens[] = {
    "generic",     "presentation", "none",     "button", "link",     "heading",
    "img",         "textbox",      "checkbox", "radio",  "switch",   "slider",
    "progressbar", "list",         "listitem", "tab",    "tabpanel", "dialog",
    "menu",        "menuitem",     "tooltip",  "alert",
};

constexpr bool prop_names_are_unique(std::span<const PropRow> list) {
    for (std::size_t i = 0; i < list.size(); ++i) {
        for (std::size_t j = i + 1; j < list.size(); ++j) {
            if (list[i].name == list[j].name) {
                return false;
            }
        }
    }
    return true;
}

static_assert(prop_names_are_unique(kCommonProps), "common prop names are unique");
static_assert(std::size(kRoleTokens) == 22, "the role token list of common-and-events.md 1.1");

} // namespace

std::span<const PropRow> common_prop_table() {
    return rows(kCommonProps);
}

std::span<const AliasAttr> common_alias_attr_table() {
    return rows(kCommonAliasAttrs);
}

std::span<const std::string_view> role_token_table() {
    return rows(kRoleTokens);
}

} // namespace tessera::schema::internal
