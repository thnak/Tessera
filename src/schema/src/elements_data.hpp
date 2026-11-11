// SPDX-License-Identifier: MIT

// The single element row table (docs/specs/jsx-intrinsics-v1/README.md section 3). The rows
// reference the per-spec data arrays in elements_basic_data.hpp, elements_other_data.hpp and
// elements_virtual_data.hpp, which keeps every file under the 500-line hard limit.

#pragma once

#include "elements_basic_data.hpp"
#include "elements_other_data.hpp"
#include "elements_virtual_data.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace tessera::schema::internal {
inline constexpr ElementRow kElements[] = {
    {"View",
     "View",
     ChildrenKind::TextAndNodes,
     "View",
     "generic",
     true,
     true,
     {},
     data::kViewEvents,
     data::kViewConstraints,
     "container; children are laid out by the style engine"},
    {"Text",
     "Text",
     ChildrenKind::Text,
     "Text",
     "text",
     true,
     true,
     {},
     data::kViewEvents,
     {},
     "one text run; an element child is rejected"},
    {"Button", "Button", ChildrenKind::TextAndNodes, "Button", "button", true, true,
     data::kButtonProps, data::kButtonEvents, data::kButtonConstraints,
     "text-only children become props.label; with an element child each run is a synthetic Text"},
    {"TextInput", "TextInput", ChildrenKind::None, "TextInput", "textbox", true, true,
     data::kTextInputProps, data::kTextInputEvents, data::kTextInputConstraints,
     "leaf; children are rejected"},
    {"TextArea", "TextArea", ChildrenKind::None, "TextArea", "textbox", true, true,
     data::kTextInputProps, data::kTextInputEvents, data::kTextInputConstraints,
     "multi-line; rows and wrap exist, secure does not"},
    {"Image", "Image", ChildrenKind::None, "Image", "img", true, true, data::kImageProps,
     data::kImageEvents, data::kImageConstraints, "alt=\"\" makes it decorative"},
    {"Path", "Path", ChildrenKind::None, "Path", "img", true, true, data::kPathProps,
     data::kPathEvents, data::kPathConstraints,
     "role img only when label is present; otherwise decorative"},
    {"Scroll",
     "Scroll",
     ChildrenKind::Nodes,
     "Scroll",
     "scroll view",
     true,
     true,
     data::kScrollProps,
     data::kScrollEvents,
     {},
     "the only built-in scroll container"},
    {"Spacer",
     "Spacer",
     ChildrenKind::None,
     "",
     "presentation",
     false,
     false,
     data::kSpacerProps,
     {},
     data::kSpacerConstraints,
     "always decorative; no className or style"},
    {"VirtualList", "VirtualList", ChildrenKind::Template, "VirtualList", "list", true, true,
     data::kVirtualListProps, data::kVirtualEvents, data::kVirtualListConstraints,
     "exactly one `.map` template child"},
    {"VirtualGrid", "VirtualGrid", ChildrenKind::Template, "VirtualGrid", "list", true, true,
     data::kVirtualGridProps, data::kVirtualEvents, data::kVirtualGridConstraints,
     "exactly one `.map` template child"},
    {"ErrorBoundary", "ErrorBoundary", ChildrenKind::One, "", "", false, false,
     data::kErrorBoundaryProps, data::kErrorBoundaryEvents, data::kErrorBoundaryConstraints,
     "not painted, no layout effect"},
    {"CustomElement",
     "CustomElement",
     ChildrenKind::None,
     "CustomElement",
     "",
     true,
     true,
     data::kCustomElementProps,
     data::kCustomElementEvents,
     {},
     "manifest ui/elements.json supplies props, events and semantics"},
};

constexpr bool element_rows_are_valid(std::span<const ElementRow> list) {
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].tag.empty() || list[i].ir_kind.empty()) {
            return false;
        }
        for (std::size_t j = i + 1; j < list.size(); ++j) {
            if (list[i].tag == list[j].tag) {
                return false;
            }
        }
        for (const PropRow& prop : list[i].props) {
            if (prop.name.empty() || prop.lowers_to.empty()) {
                return false;
            }
        }
        for (const EventRow& event : list[i].events) {
            if (event.name.empty()) {
                return false;
            }
        }
        for (const ConstraintRow& constraint : list[i].constraints) {
            if (constraint.code.empty()) {
                return false;
            }
        }
    }
    return true;
}

static_assert(std::size(kElements) == 13, "the intrinsic list of README section 3 has 13 tags");

static_assert(element_rows_are_valid(kElements), "element rows have names and lowering targets");

} // namespace tessera::schema::internal
