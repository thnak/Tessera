// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/node.hpp"

#include <cstdint>

// Per-kind facts about `ir::Node`, in one table (god-type audit section 4.3).
//
// `ir::Node` carries the union of every kind's fields, so validation and lowering repeatedly ask
// "does this kind have `element`?", "may it have children?", "is it a virtual collection?" - as
// ad-hoc `kind == NodeKind::X || kind == NodeKind::Y` predicates in `validate_node.cpp`,
// `node_lowering_internal.hpp` and the emitters. A kind added to the enum then has to be found in
// every one of those places, and the compiler cannot say when one was missed.
//
// The table is keyed by `NodeKind`, so a new kind is a missing row: a compile error, not a silently
// wrong predicate. It deliberately says nothing about the wire format - the IR encoder keeps
// writing the same bytes (section 4.3's risk note), and `kNodeKindTraits` is a build-time constant
// with no versioning footprint.
namespace tessera::compiler::ir {

// The field groups a kind may carry. These are the questions the validators already ask, named once
// instead of re-derived; anything a kind may *not* carry is reported by the checker that owns that
// field, using `allows()`.
struct NodeKindTraits {
    const char* name = "";
    // The kind may carry `element` (CustomElement only).
    bool has_element : 1 = false;
    // The kind may carry `component` (Component only).
    bool has_component : 1 = false;
    // The kind may carry `text` (Text only).
    bool has_text : 1 = false;
    // The kind may carry `item_template` (virtual collections only).
    bool has_item_template : 1 = false;
    // The kind may carry `fallback` (ErrorBoundary, Suspense).
    bool has_fallback : 1 = false;
    // The kind may carry `style` (every kind except Component and Slot).
    bool has_style : 1 = true;
    // The kind is a leaf: no children (CustomElement, TextInput, TextArea, Image, Path, Spacer).
    bool is_leaf : 1 = false;
    // The kind is a virtual collection (VirtualList, VirtualGrid).
    bool is_collection : 1 = false;
    // The kind accepts the caller's slot content as children (Component).
    bool takes_slot_children : 1 = false;
    // The kind may appear as an inline span child of a Text (Text).
    bool is_inline_span : 1 = false;
    // The kind is an input that owns editable state (TextInput, TextArea).
    bool is_text_input : 1 = false;
};

// One row per `NodeKind`, indexed by the enum. `NodeKind::View` is 0 and `Slot` is last, so the
// row order here must match the declaration order in node.hpp - checked by the static_assert in
// node_kind_traits.cpp.
inline constexpr NodeKindTraits kNodeKindTraits[] = {
    //                         name              elem   comp   text   item   fall   style  leaf coll
    //                         slotCh span   input
    {"View", false, false, false, false, false, true, false, false, false, false, false},
    {"Text", false, false, true, false, false, true, false, false, false, true, false},
    {"Button", false, false, false, false, false, true, false, false, false, false, false},
    {"TextInput", false, false, false, false, false, true, true, false, false, false, true},
    {"TextArea", false, false, false, false, false, true, true, false, false, false, true},
    {"Image", false, false, false, false, false, true, true, false, false, false, false},
    {"Path", false, false, false, false, false, true, true, false, false, false, false},
    {"Scroll", false, false, false, false, false, true, false, false, false, false, false},
    {"Spacer", false, false, false, false, false, true, true, false, false, false, false},
    {"VirtualList", false, false, false, true, false, true, false, true, false, false, false},
    {"VirtualGrid", false, false, false, true, false, true, false, true, false, false, false},
    {"ErrorBoundary", false, false, false, false, true, true, false, false, false, false, false},
    {"Suspense", false, false, false, false, true, true, false, false, false, false, false},
    {"CustomElement", true, false, false, false, false, true, true, false, false, false, false},
    {"Component", false, true, false, false, false, false, false, false, true, false, false},
    {"Slot", false, false, false, false, false, false, true, false, false, false, false},
};

inline constexpr std::size_t kNodeKindCount = sizeof(kNodeKindTraits) / sizeof(kNodeKindTraits[0]);

// The traits of `kind`. Every enumerator has a row, so this is total up to a corrupted enum value.
[[nodiscard]] constexpr const NodeKindTraits& node_kind_traits(NodeKind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index < kNodeKindCount ? kNodeKindTraits[index] : kNodeKindTraits[0];
}

} // namespace tessera::compiler::ir
