// SPDX-License-Identifier: MIT

// The attribute rewrites of the lowercase HTML tag aliases (docs/specs/jsx-intrinsics-v1/
// html-aliases.md section 3), split from aliases.cpp so neither file outgrows the 500-line limit.

#pragma once

#include "tables.hpp"

#include <cstddef>
#include <span>

namespace tessera::schema::internal::alias_data {

template<typename Row, std::size_t N> constexpr std::span<const Row> rows(const Row (&list)[N]) {
    return std::span<const Row>(list);
}

// --- attribute rewrites ---------------------------------------------------------------------

inline constexpr AliasValue kButtonTypeValues[] = {
    {"button", "", false, ""},
    {"submit", "", true, "V1 has no form element; use `onClick`"},
    {"reset", "", true, "V1 has no form element; use `onClick`"},
};

inline constexpr AliasAttr kButtonAttrs[] = {
    {"type", AliasAttrKind::ValueMap, "", "V1 has no form element; use `onClick`",
     rows(kButtonTypeValues), "must be a string literal"},
};

inline constexpr AliasValue kWrapValues[] = {
    {"soft", "word", false, ""},
    {"off", "none", false, ""},
    {"hard", "", true, "HTML hard wrapping has no V1 equivalent"},
};

inline constexpr AliasAttr kTextAreaAttrs[] = {
    {"wrap", AliasAttrKind::ValueMap, "", "HTML hard wrapping has no V1 equivalent",
     rows(kWrapValues), ""},
    {"cols", AliasAttrKind::Reject, "", "set a width in `style`", {}, ""},
};

inline constexpr AliasAttr kInputAttrs[] = {
    {"autoComplete",
     AliasAttrKind::Rename,
     "autofill",
     "",
     {},
     "tokens email/username/current-password/new-password/one-time-code/name/tel/"
     "street-address/postal-code pass; `off` is dropped; any other token is HTML_ATTR [Q19]"},
    {"checked",
     AliasAttrKind::Reject,
     "",
     "form and validation belong to library `Form`/`Field`",
     {},
     ""},
    {"defaultChecked",
     AliasAttrKind::Reject,
     "",
     "form and validation belong to library `Form`/`Field`",
     {},
     ""},
    {"name",
     AliasAttrKind::Reject,
     "",
     "form and validation belong to library `Form`/`Field`",
     {},
     ""},
    {"required",
     AliasAttrKind::Reject,
     "",
     "form and validation belong to library `Form`/`Field`",
     {},
     ""},
    {"pattern",
     AliasAttrKind::Reject,
     "",
     "form and validation belong to library `Form`/`Field`",
     {},
     ""},
    {"min",
     AliasAttrKind::Reject,
     "",
     "form and validation belong to library `Form`/`Field`",
     {},
     ""},
    {"max",
     AliasAttrKind::Reject,
     "",
     "form and validation belong to library `Form`/`Field`",
     {},
     ""},
    {"step",
     AliasAttrKind::Reject,
     "",
     "form and validation belong to library `Form`/`Field`",
     {},
     ""},
    {"minLength",
     AliasAttrKind::Reject,
     "",
     "use `maxLength`; minimum length is a form rule",
     {},
     ""},
    {"multiple", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"accept", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"list", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"form", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"size", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"autoFocus",
     AliasAttrKind::Reject,
     "",
     "not in V1 on inputs; use it on a focusable `View`",
     {},
     ""},
    {"spellCheck", AliasAttrKind::Reject, "", "not in V1", {}, ""},
    {"autoCapitalize", AliasAttrKind::Reject, "", "not in V1", {}, ""},
    {"autoCorrect", AliasAttrKind::Reject, "", "not in V1", {}, ""},
};

inline constexpr AliasAttr kImgAttrs[] = {
    {"width",
     AliasAttrKind::ToStyle,
     "width",
     "",
     {},
     "a number or numeric string is dp; with `style.width` it is TESSERA_JSX_PROP_CONFLICT"},
    {"height",
     AliasAttrKind::ToStyle,
     "height",
     "",
     {},
     "a number or numeric string is dp; with `style.height` it is TESSERA_JSX_PROP_CONFLICT"},
    {"srcSet", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"sizes", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"loading", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"decoding", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"crossOrigin", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"onLoad", AliasAttrKind::Reject, "", "no V1 meaning", {}, ""},
    {"onError", AliasAttrKind::Reject, "", "no V1 meaning; use ErrorBoundary", {}, ""},
};

// `ol start`/`reversed` and `li value` number the markers of the list (docs/design/list-style.md
// section 2). They are carried as `aria-*` pairs on the node, which the marker painter reads.
inline constexpr AliasAttr kOrderedListAttrs[] = {
    {"start", AliasAttrKind::Rename, "aria-start", "", {}, "a static integer"},
    {"reversed", AliasAttrKind::Rename, "aria-reversed", "", {}, "a bare flag or a static boolean"},
};

inline constexpr AliasAttr kListItemAttrs[] = {
    {"value", AliasAttrKind::Rename, "aria-value", "", {}, "a static integer"},
};

// Table spans (docs/design/table-layout.md section 2) are carried as `aria-colspan`/`aria-rowspan`,
// the ARIA names of the same facts; the layout input and the accessibility tree both read them.
inline constexpr AliasAttr kCellAttrs[] = {
    {"colSpan", AliasAttrKind::Rename, "aria-colspan", "", {}, "a static integer"},
    {"rowSpan",
     AliasAttrKind::Rename,
     "aria-rowspan",
     "",
     {},
     "a static integer; 0 spans the group"},
};

inline constexpr AliasAttr kHeaderCellAttrs[] = {
    {"colSpan", AliasAttrKind::Rename, "aria-colspan", "", {}, "a static integer"},
    {"rowSpan",
     AliasAttrKind::Rename,
     "aria-rowspan",
     "",
     {},
     "a static integer; 0 spans the group"},
    {"scope",
     AliasAttrKind::Reject,
     "",
     "row headers are not modelled; `th` is a column header",
     {},
     ""},
};

inline constexpr AliasAttr kColumnAttrs[] = {
    {"span", AliasAttrKind::Rename, "aria-colspan", "", {}, "a static integer"},
};

// `href` is the only attribute with behavior (issue #542): an absolute route path goes through the
// navigation service, a URL with an allowed scheme through the shell service. `target` and `rel`
// describe browser window and privacy behavior the engine does not have, so the values a port
// writes everywhere are accepted and dropped; any other value is rejected.
inline constexpr AliasValue kAnchorTargetValues[] = {
    {"_self", "", false, ""},
    {"_blank", "", false, ""},
};

inline constexpr AliasValue kAnchorRelValues[] = {
    {"noopener", "", false, ""},
    {"noreferrer", "", false, ""},
    {"noopener noreferrer", "", false, ""},
    {"nofollow", "", false, ""},
    {"external", "", false, ""},
};

inline constexpr AliasAttr kAnchorAttrs[] = {
    {"href", AliasAttrKind::Rename, "aria-href", "", {}, "a static string of at most 128 bytes"},
    {"target", AliasAttrKind::ValueMap, "",
     "an external URL always opens through the shell service", rows(kAnchorTargetValues),
     "must be a string literal"},
    {"rel", AliasAttrKind::ValueMap, "", "an external URL always opens through the shell service",
     rows(kAnchorRelValues), "must be a string literal"},
    {"download",
     AliasAttrKind::Reject,
     "",
     "no V1 meaning; use the shell service's save dialog",
     {},
     ""},
};

inline constexpr AliasAttr kLabelAttrs[] = {
    {"htmlFor", AliasAttrKind::Rename, "aria-for", "", {}, "a static string naming a control's id"},
};

} // namespace tessera::schema::internal::alias_data
