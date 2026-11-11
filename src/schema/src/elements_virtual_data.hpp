// SPDX-License-Identifier: MIT

// Table data of the intrinsic elements, split by element spec so no file outgrows the 500-line
// hard limit. Included by the single element row table in elements_data.hpp.

#pragma once

#include "data_helpers.hpp"

namespace tessera::schema::internal::data {
inline constexpr EventRow kVirtualEvents[] = {
    {"onEndReached", "", "throttled; no payload; runs on the UI thread"},
    {"onScroll", "ScrollEvent", ""},
    {"onSizeChange", "SizeChangeEvent", "layout observation; no bubbling, not a pointer event"},
};

inline constexpr EventRow kErrorBoundaryEvents[] = {
    {"onError", "ErrorInfo", ""},
};

inline constexpr EventRow kCustomElementEvents[] = {
    {"onPointer", "", "manifest input `pointer`; payload from the manifest"},
    {"onKey", "", "manifest input `key`; payload from the manifest"},
    {"onWheel", "", "manifest input `wheel`; payload from the manifest"},
    {"onText", "", "manifest input `text`; payload from the manifest"},
    {"onGamepad", "", "manifest input `gamepad`; payload from the manifest"},
    {"onSizeChange", "SizeChangeEvent", "layout observation; no bubbling, not a pointer event"},
};

inline constexpr std::string_view kItemSizeOnlyOn[] = {"VirtualList"};
inline constexpr std::string_view kGridPropsOnlyOn[] = {"VirtualGrid"};

inline constexpr PropRow kVirtualListProps[] = {
    {"itemSize",
     "length",
     ValueKind::Expression,
     false,
     "",
     "props.itemSize",
     tags(kItemSizeOnlyOn),
     {},
     "fixed main-axis size; O(1) range"},
    {"estimateSize",
     "length",
     ValueKind::Expression,
     false,
     "",
     "props.estimateSize",
     {},
     {},
     "variable size; the default estimate is unspecified"},
    {"overscan",
     "length",
     ValueKind::Expression,
     false,
     "ui.virtualListOverscanPx (200)",
     "props.overscan",
     {},
     {},
     "dp realized beyond the viewport"},
    {"gap",
     "length",
     ValueKind::Expression,
     false,
     "0",
     "props.gap",
     {},
     {},
     "between items and between grid rows and columns"},
    {"orientation",
     "\"vertical\" | \"horizontal\"",
     ValueKind::Literal,
     false,
     "vertical",
     "props.orientation",
     {},
     {},
     ""},
};

inline constexpr PropRow kVirtualGridProps[] = {
    {"estimateSize",
     "length",
     ValueKind::Expression,
     false,
     "",
     "props.estimateSize",
     {},
     {},
     "variable size; the default estimate is unspecified"},
    {"rowHeight",
     "length",
     ValueKind::Expression,
     false,
     "",
     "props.rowHeight",
     tags(kGridPropsOnlyOn),
     {},
     "fixed row height"},
    {"columns",
     "int",
     ValueKind::Expression,
     false,
     "",
     "props.columns",
     tags(kGridPropsOnlyOn),
     {},
     "fixed column count, at least 1"},
    {"columnWidth",
     "length",
     ValueKind::Expression,
     false,
     "",
     "props.columnWidth",
     tags(kGridPropsOnlyOn),
     {},
     "fixed column width"},
    {"overscan",
     "length",
     ValueKind::Expression,
     false,
     "ui.virtualListOverscanPx (200)",
     "props.overscan",
     {},
     {},
     "dp realized beyond the viewport"},
    {"gap",
     "length",
     ValueKind::Expression,
     false,
     "0",
     "props.gap",
     {},
     {},
     "between items and between grid rows and columns"},
    {"orientation",
     "\"vertical\" | \"horizontal\"",
     ValueKind::Literal,
     false,
     "vertical",
     "props.orientation",
     {},
     {},
     ""},
};

inline constexpr PropRow kErrorBoundaryProps[] = {
    {"fallback",
     "one element",
     ValueKind::Element,
     true,
     "",
     "node.fallback",
     {},
     {},
     "intrinsic or component element; not conditional, loop, fragment or text"},
    {"resetKey",
     "string | int | bool",
     ValueKind::Expression,
     false,
     "",
     "props.resetKey",
     {},
     {},
     "any change resets the boundary"},
    {"maxAutoRetries",
     "int",
     ValueKind::Literal,
     false,
     "0",
     "props.maxAutoRetries",
     {},
     {},
     "at least 0; bounded and logged"},
    {"catchEvents", "bool", ValueKind::Literal, false, "true", "props.catchEvents", {}, {}, ""},
};

inline constexpr PropRow kCustomElementProps[] = {
    {"element",
     "string",
     ValueKind::Literal,
     true,
     "",
     "node.element",
     {},
     {},
     "an id of ui/elements.json; no expression"},
    {"props",
     "object literal",
     ValueKind::Literal,
     false,
     "",
     "props.<key>",
     {},
     {},
     "keys are literal manifest props, values are expressions"},
};

inline constexpr std::string_view kListSizeProps[] = {"itemSize", "estimateSize"};
inline constexpr std::string_view kGridSizeProps[] = {"rowHeight", "estimateSize"};
inline constexpr std::string_view kGridColumnProps[] = {"columns", "columnWidth"};
inline constexpr ConstraintRow kVirtualListConstraints[] = {
    {ConstraintKind::OneOf, tags(kListSizeProps), false, "TESSERA_JSX_PROP_ONE_OF",
     "exactly one of itemSize and estimateSize"},
};

inline constexpr ConstraintRow kVirtualGridConstraints[] = {
    {ConstraintKind::OneOf, tags(kGridSizeProps), false, "TESSERA_JSX_PROP_ONE_OF",
     "exactly one of rowHeight and estimateSize"},
    {ConstraintKind::OneOf, tags(kGridColumnProps), false, "TESSERA_JSX_PROP_ONE_OF",
     "exactly one of columns and columnWidth"},
};

inline constexpr ConstraintRow kErrorBoundaryConstraints[] = {
    {ConstraintKind::Custom,
     {},
     false,
     "TESSERA_JSX_CHILDREN_COUNT",
     "exactly one element child; wrap siblings in a View"},
};
} // namespace tessera::schema::internal::data