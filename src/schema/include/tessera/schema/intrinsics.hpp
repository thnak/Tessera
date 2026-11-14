// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

// The machine-readable intrinsic table of docs/specs/jsx-intrinsics-v1 (Option C of
// machine-readable.md): one hand-written constexpr source shared by analysis, lowering, the IR
// validator, the designer, the runtime node factory and the typings generator.
//
// Layering: this library is a leaf. It uses the C++ standard library only and must not include
// UI, rendering, JSX, IR or HAL headers. Tables are UTF-8 string_views over string literals; the
// lookups allocate nothing.
//
// Rows match the review authority (docs/specs/jsx-intrinsics-v1/*.md). Units: `PropRow::lowers_to`
// uses the vocabulary of machine-readable.md section 3 (`props.<n>`, `style.<n>`, `key`, `refs`,
// `events.<n>`, `automationId`, `children`, `node.<field>`); `default_value` is the spec text
// (`false`, `0`, `"contain"` without quotes, `ui.virtualListOverscanPx (200)`, empty when the prop
// is absent in IR).
namespace tessera::schema {

// Version of the table layout (machine-readable.md section 5, "Versioning").
inline constexpr int kSchemaVersion = 1;

// README section 2, "children": the five rules of the element table plus `text+nodes` and
// `inline-text` (a `Text` accepts text runs and inline spans, elements-basic.md section 2.1).
enum class ChildrenKind : std::uint8_t {
    None,
    Text,
    InlineText,
    Nodes,
    TextAndNodes,
    Template, // exactly one list-loop template (VirtualList/VirtualGrid)
    One,      // exactly one element (ErrorBoundary)
};

// README section 2, "Value": L, E, H, N. `Name` is the class of the common `ref` row (a `useRef`
// name rather than an expression); the spec table writes it as "name".
enum class ValueKind : std::uint8_t { Literal, Expression, Handler, Element, Name };

// The closed constraint list of machine-readable.md section 3.
enum class ConstraintKind : std::uint8_t {
    Requires,      // every entry of `props` must be present
    Exclusive,     // at most one entry of `props` may be present
    OneOf,         // exactly one entry of `props`, or the children when `or_children`
    ContentOneOf,  // one entry of `props` is the content, or the children when `or_children`
    OnlyOn,        // the prop is restricted to the tags named by `props`
    WarnIfMissing, // `props[0]` without any of `props[1..]` is a warning with `code`
    Custom,        // a rule the analysis pass implements; `note` and `code` name it
};

enum class AliasStatus : std::uint8_t { Mapped, Partial, Unsupported };

// Attribute rewrites of an alias (html-aliases.md section 3).
enum class AliasAttrKind : std::uint8_t {
    Keep,    // unchanged; the target's table applies
    Rename,  // `rename` is the intrinsic attribute name
    Reject,  // no V1 meaning; `suggestion` is the alternative
    ToStyle, // becomes `style.<rename>` (numeric or numeric-string value)
    ValueMap // `values` maps the HTML literal to a replacement or a rejection
};

// Kind of a style property (machine-readable.md section 5): a number in an inline `style` object
// becomes "<n>px" for `Length` and stays unitless for `Number` (decision 10).
enum class StyleKind : std::uint8_t { Length, Number, Color, Keyword, List };

[[nodiscard]] std::string_view to_string(ChildrenKind kind);
[[nodiscard]] std::string_view to_string(ValueKind kind);
[[nodiscard]] std::string_view to_string(ConstraintKind kind);
[[nodiscard]] std::string_view to_string(AliasStatus status);
[[nodiscard]] std::string_view to_string(AliasAttrKind kind);
[[nodiscard]] std::string_view to_string(StyleKind kind);

// common-and-events.md section 2: an event payload record and its fields.
struct TypeField {
    std::string_view name;
    std::string_view type;
};

struct TypeRecord {
    std::string_view name;
    std::span<const TypeField> fields;
    std::string_view primary; // the member a setter reference receives; empty when none
    std::string_view note;
};

// One prop row of an element table or of the common table.
struct PropRow {
    std::string_view name;
    std::string_view type;
    ValueKind value = ValueKind::Expression;
    bool required = false;
    std::string_view default_value;
    std::string_view lowers_to;
    std::span<const std::string_view> only_on; // tags this prop is restricted to; empty = all
    std::span<const std::string_view> not_on;  // tags that reject it (common table)
    std::string_view note;
};

struct EventRow {
    std::string_view name;
    std::string_view payload; // a TypeRecord name; empty when the manifest supplies it
    std::string_view note;
};

// One constraint row. See ConstraintKind for how `props` is read; `or_children` applies to OneOf
// and ContentOneOf.
struct ConstraintRow {
    ConstraintKind kind = ConstraintKind::Custom;
    std::span<const std::string_view> props;
    bool or_children = false;
    std::string_view code;
    std::string_view note;
};

struct ElementRow {
    std::string_view tag;
    std::string_view ir_kind;
    ChildrenKind children = ChildrenKind::None;
    std::string_view ref_type; // the `useRef` element type; empty = `ref` rejected
    std::string_view implied_role;
    bool accepts_class_name = true;
    bool accepts_style = true;
    std::span<const PropRow> props;
    std::span<const EventRow> events;
    std::span<const ConstraintRow> constraints;
    std::string_view note;
};

// A prop injected by an attribute rewrite (`ul` -> role="list", `input type="password"` -> secure).
struct AliasInject {
    std::string_view name;
    std::string_view value;
};

// The default inline style an emphasis alias injects when it is used as an inline span
// (html-aliases.md section 2; elements-basic.md section 2.1). `property` is a schema style name.
struct AliasStyle {
    std::string_view tag;
    std::string_view property;
    std::string_view value;
};

// One value of a ValueMap attribute (`button type`, `textarea wrap`).
struct AliasValue {
    std::string_view value;
    std::string_view replacement; // empty when the value is rejected
    bool rejected = false;
    std::string_view suggestion;
};

struct AliasAttr {
    std::string_view name;
    AliasAttrKind kind = AliasAttrKind::Keep;
    std::string_view rename;     // Rename: intrinsic name; ToStyle: style property
    std::string_view suggestion; // Reject and ValueMap rejections
    std::span<const AliasValue> values;
    std::string_view note;
};

// `input type="<type>"` -> injected intrinsic props (html-aliases.md section 3.2).
struct AliasTypeMap {
    std::string_view type;
    std::span<const AliasInject> inject;
};

// One lowercase tag. A tag with no mapping has status Unsupported, an empty `target` and the
// `alternative` text of html-aliases.md section 5. Section 5 groups are expanded to one row per
// tag so machines need no group lookup.
struct AliasRow {
    std::string_view tag;
    std::string_view target; // intrinsic tag; empty when unsupported
    AliasStatus status = AliasStatus::Unsupported;
    std::span<const AliasInject> inject;
    std::span<const AliasAttr> attrs;
    std::span<const AliasTypeMap> type_map;
    bool type_map_closed = false; // an unlisted `type` literal is rejected
    std::string_view alternative;
    std::string_view note;
};

struct DomType {
    std::string_view name;   // accepted in `useRef<...>`
    std::string_view target; // intrinsic tag
};

// Common and reserved attributes (common-and-events.md section 1), shared by every element.
[[nodiscard]] std::span<const PropRow> common_props();
// Attribute rewrites that apply to every lowercase alias (html-aliases.md section 3.1).
[[nodiscard]] std::span<const AliasAttr> common_alias_attrs();
// WAI-ARIA role tokens accepted by `role` (common-and-events.md section 1.1).
[[nodiscard]] std::span<const std::string_view> role_tokens();

[[nodiscard]] std::span<const TypeRecord> type_records();
[[nodiscard]] std::span<const ElementRow> elements();
[[nodiscard]] std::span<const AliasRow> aliases(); // mapped, partial and unsupported rows
// The injected default run styles of the emphasis aliases (`b`, `strong`, `i`, `em`, `code`), in
// table order; a tag without an entry gets none.
[[nodiscard]] std::span<const AliasStyle> alias_style_defaults(std::string_view tag);
[[nodiscard]] std::span<const DomType> dom_types();

// Null when the tag is not an intrinsic or the name is not a payload record.
[[nodiscard]] const ElementRow* find_element(std::string_view tag);
[[nodiscard]] const TypeRecord* find_type_record(std::string_view name);
// Mapped, partial and unsupported lowercase tags; null for an unknown tag.
[[nodiscard]] const AliasRow* find_alias(std::string_view tag);
// A DOM element type accepted in `useRef<...>`; null when unknown.
[[nodiscard]] const DomType* find_dom_type(std::string_view name);

} // namespace tessera::schema
