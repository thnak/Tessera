// SPDX-License-Identifier: MIT

// Table data of the intrinsic elements, split by element spec so no file outgrows the 500-line
// hard limit. Included by the single element row table in elements_data.hpp.

#pragma once

#include "data_helpers.hpp"

namespace tessera::schema::internal::data {
inline constexpr EventRow kScrollEvents[] = {
    {"onScroll", "ScrollEvent", ""},
    {"onPointerDown", "PointerEvent", ""},
    {"onPointerUp", "PointerEvent", ""},
    {"onPointerMove", "PointerEvent", ""},
    {"onPointerEnter", "PointerEvent", ""},
    {"onPointerLeave", "PointerEvent", ""},
    {"onPointerCancel", "PointerEvent", ""},
    {"onSizeChange", "SizeChangeEvent", "layout observation; no bubbling, not a pointer event"},
};

inline constexpr EventRow kImageEvents[] = {
    {"onSizeChange", "SizeChangeEvent", "layout observation; no bubbling, not a pointer event"},
};

inline constexpr EventRow kPathEvents[] = {
    {"onSizeChange", "SizeChangeEvent", "layout observation; no bubbling, not a pointer event"},
};

inline constexpr PropRow kImageProps[] = {
    {"src",
     "string",
     ValueKind::Literal,
     true,
     "",
     "props.src",
     {},
     {},
     "project-relative literal path; the file must exist and be PNG/JPEG/WebP"},
    {"alt",
     "string",
     ValueKind::Expression,
     true,
     "",
     "props.alt",
     {},
     {},
     "required even though it is an expression; empty means decorative"},
    {"fit",
     "\"contain\" | \"cover\" | \"fill\" | \"none\"",
     ValueKind::Literal,
     false,
     "contain",
     "props.fit",
     {},
     {},
     ""},
};

inline constexpr PropRow kPathProps[] = {
    {"d",
     "string",
     ValueKind::Literal,
     true,
     "",
     "props.d",
     {},
     {},
     "SVG path data; parsed by the analysis pass"},
    {"viewBox",
     "string",
     ValueKind::Literal,
     true,
     "",
     "props.viewBox",
     {},
     {},
     "four numbers; width and height greater than 0"},
    {"fill",
     "color | var(--name) | \"none\"",
     ValueKind::Literal,
     false,
     "none",
     "props.fill",
     {},
     {},
     ""},
    {"stroke",
     "color | var(--name) | \"none\"",
     ValueKind::Literal,
     false,
     "none",
     "props.stroke",
     {},
     {},
     ""},
    {"strokeWidth",
     "number",
     ValueKind::Literal,
     false,
     "1",
     "props.strokeWidth",
     {},
     {},
     "viewBox units; ignored when stroke is none"},
    {"label",
     "string",
     ValueKind::Expression,
     false,
     "",
     "props.label",
     {},
     {},
     "accessible name; without it the path is decorative"},
};

inline constexpr PropRow kScrollProps[] = {
    {"axis",
     "\"y\" | \"x\" | \"both\"",
     ValueKind::Literal,
     false,
     "y",
     "props.axis",
     {},
     {},
     "scroll axes [Q10]"},
};

inline constexpr PropRow kSpacerProps[] = {
    {"size",
     "length",
     ValueKind::Literal,
     false,
     "",
     "props.size",
     {},
     {},
     "fixed main-axis size; one of size or grow"},
    {"grow",
     "number",
     ValueKind::Literal,
     false,
     "0",
     "props.grow",
     {},
     {},
     "share of the free main-axis space; 0 or greater"},
};

inline constexpr std::string_view kSpacerSizeProps[] = {"size", "grow"};
inline constexpr ConstraintRow kImageConstraints[] = {
    {ConstraintKind::Custom,
     {},
     false,
     "TESSERA_JSX_IMAGE_SRC",
     "the src file must exist, be PNG/JPEG/WebP and within the resource limits"},
    {ConstraintKind::Custom, {}, false, "TESSERA_JSX_IMAGE_ALT", "alt is required"},
    {ConstraintKind::Custom, {}, false, "TESSERA_JSX_IMAGE_FORMAT", ""},
    {ConstraintKind::Custom, {}, false, "TESSERA_JSX_IMAGE_SIZE", ""},
};

inline constexpr ConstraintRow kPathConstraints[] = {
    {ConstraintKind::Custom, {}, false, "TESSERA_JSX_PATH_DATA", "d must parse as SVG path data"},
};

inline constexpr ConstraintRow kSpacerConstraints[] = {
    {ConstraintKind::OneOf, tags(kSpacerSizeProps), false, "TESSERA_JSX_SPACER_EMPTY",
     "a Spacer needs exactly one of size and grow"},
};

} // namespace tessera::schema::internal::data