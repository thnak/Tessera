// SPDX-License-Identifier: MIT
//
// Compile-time checks on the `NodeKind` traits table (god-type audit section 4.3).
//
// The table is indexed by the enum, so the only way it can be wrong is being *misaligned* - a row
// out of order or missing - and that is exactly what a compile-time check can prove. The runtime
// behaviour tests live with the validators that consume the table.

#include "tessera/compiler/ir/node_kind_traits.hpp"

namespace tessera::compiler::ir {
namespace {

// One row per kind, and `Slot` last: the table cannot be short or long without failing here.
static_assert(kNodeKindCount == static_cast<std::size_t>(NodeKind::Slot) + 1,
              "kNodeKindTraits needs exactly one row per NodeKind, in declaration order");

// The names must match `node_kind_name`, which is what the validators print in diagnostics. A
// misaligned row would otherwise report the wrong kind's name for a real error.
static_assert(kNodeKindTraits[static_cast<std::size_t>(NodeKind::View)].name ==
                  std::string_view("View"),
              "row 0 is View");
static_assert(kNodeKindTraits[static_cast<std::size_t>(NodeKind::Text)].name ==
                  std::string_view("Text"),
              "row 1 is Text");
static_assert(kNodeKindTraits[static_cast<std::size_t>(NodeKind::TextInput)].name ==
                  std::string_view("TextInput"),
              "row 3 is TextInput");
static_assert(kNodeKindTraits[static_cast<std::size_t>(NodeKind::VirtualList)].name ==
                  std::string_view("VirtualList"),
              "row 9 is VirtualList");
static_assert(kNodeKindTraits[static_cast<std::size_t>(NodeKind::CustomElement)].name ==
                  std::string_view("CustomElement"),
              "row 12 is CustomElement");
static_assert(kNodeKindTraits[static_cast<std::size_t>(NodeKind::Slot)].name ==
                  std::string_view("Slot"),
              "the last row is Slot");

// The field-group columns are the questions the validators ask, so a misaligned column is a silent
// behavioural change. These spot-check the kinds whose answers are load-bearing.
static_assert(node_kind_traits(NodeKind::CustomElement).has_element,
              "a CustomElement has `element`");
static_assert(!node_kind_traits(NodeKind::View).has_element, "a View has no `element`");
static_assert(node_kind_traits(NodeKind::Component).has_component, "a Component has `component`");
static_assert(!node_kind_traits(NodeKind::Component).has_style,
              "a Component carries props, not style (section 3.4)");
static_assert(node_kind_traits(NodeKind::Component).takes_slot_children,
              "a Component accepts the caller's slot content");
static_assert(node_kind_traits(NodeKind::Text).has_text, "a Text has `text`");
static_assert(node_kind_traits(NodeKind::Text).is_inline_span,
              "a Text may be an inline span child");
static_assert(node_kind_traits(NodeKind::VirtualList).is_collection,
              "a VirtualList is a collection");
static_assert(node_kind_traits(NodeKind::VirtualList).has_item_template,
              "a virtual collection has an itemTemplate");
static_assert(node_kind_traits(NodeKind::ErrorBoundary).has_fallback,
              "an ErrorBoundary has a fallback");
static_assert(node_kind_traits(NodeKind::Slot).is_leaf, "a Slot is a leaf");
static_assert(!node_kind_traits(NodeKind::Slot).has_style, "a Slot carries no style");
static_assert(node_kind_traits(NodeKind::TextInput).is_text_input, "a TextInput is a text input");
static_assert(node_kind_traits(NodeKind::Image).is_leaf, "an Image is a leaf");
static_assert(!node_kind_traits(NodeKind::View).is_leaf, "a View may have children");

// Exactly two kinds are virtual collections, exactly two are text inputs, exactly one is a
// component. A count check catches a row where a *different* kind got the flag. (The flags are
// bit-fields, so the column is selected by index rather than by pointer-to-member.)
constexpr int count_flag(int column) {
    int total = 0;
    for (const NodeKindTraits& row : kNodeKindTraits) {
        const bool flags[] = {row.has_element,       row.has_component, row.has_text,
                              row.has_item_template, row.has_fallback,  row.has_style,
                              row.is_leaf,           row.is_collection, row.takes_slot_children,
                              row.is_inline_span,    row.is_text_input};
        if (flags[column]) {
            ++total;
        }
    }
    return total;
}

constexpr int kHasElement = 0;
constexpr int kHasComponent = 1;
constexpr int kHasItemTemplate = 3;
constexpr int kHasFallback = 4;
constexpr int kIsCollection = 7;
constexpr int kTakesSlotChildren = 8;
constexpr int kIsInlineSpan = 9;
constexpr int kIsTextInput = 10;

static_assert(count_flag(kIsCollection) == 2, "two virtual collections");
static_assert(count_flag(kIsTextInput) == 2, "two text inputs");
static_assert(count_flag(kHasComponent) == 1, "one component kind");
static_assert(count_flag(kHasElement) == 1, "one custom-element kind");
static_assert(count_flag(kHasFallback) == 1, "one error-boundary kind");
static_assert(count_flag(kHasItemTemplate) == 2, "one template per collection");
static_assert(count_flag(kIsInlineSpan) == 1, "one inline-span kind");
static_assert(count_flag(kTakesSlotChildren) == 1, "one slot-content kind");

} // namespace
} // namespace tessera::compiler::ir
