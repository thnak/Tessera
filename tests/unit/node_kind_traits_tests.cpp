// SPDX-License-Identifier: MIT
//
// The `NodeKind` traits table (god-type audit section 4.3), checked against the two things that
// already existed and must keep agreeing with it: `node_kind_name` and the validator's own
// diagnostics.
//
// The table's *alignment* is proved at compile time (src/compiler/ir/src/node_kind_traits.cpp): one
// row per kind, names in declaration order, and the field-group columns spot-checked with counts.
// What a compile-time check cannot prove is that the table agrees with the rest of the IR - that is
// what this test does, so a future row edit that drifts from `node_kind_name` or from the
// validator's messages fails here rather than in a user's error output.
//
// The IR wire format is untouched by the table: `ir_json` still writes the same keys for the same
// kinds, which the existing ir_json tests continue to assert.

#include "tessera/compiler/ir/node.hpp"
#include "tessera/compiler/ir/node_kind_traits.hpp"

#include <iostream>
#include <string_view>

namespace {

using tessera::compiler::ir::kNodeKindCount;
using tessera::compiler::ir::kNodeKindTraits;
using tessera::compiler::ir::node_kind_name;
using tessera::compiler::ir::node_kind_traits;
using tessera::compiler::ir::NodeKind;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "node_kind_traits: " << message << "\n";
    }
    return condition;
}

bool check_table_names_match_the_ir() {
    // `node_kind_name` is the authority the validators print in diagnostics, so the table must not
    // be a second spelling of the same vocabulary.
    bool ok = true;
    for (std::size_t index = 0; index < kNodeKindCount; ++index) {
        const auto kind = static_cast<NodeKind>(index);
        ok = expect(std::string_view(kNodeKindTraits[index].name) == node_kind_name(kind),
                    "the table's name matches node_kind_name") &&
             ok;
    }
    return ok;
}

bool check_every_kind_is_reachable() {
    // The last enumerator is Slot, so a table of the right length covers every kind exactly once.
    return expect(kNodeKindCount == static_cast<std::size_t>(NodeKind::Slot) + 1,
                  "one row per kind") &&
           expect(node_kind_traits(NodeKind::Slot).name == std::string_view("Slot"),
                  "the last row is Slot");
}

bool check_field_groups_match_the_validator() {
    // Each assertion here restates a check `validate_node.cpp` makes through the table, so the two
    // cannot drift: if a future edit flips one, either the validator's behaviour or this fails.
    return expect(node_kind_traits(NodeKind::CustomElement).has_element,
                  "CustomElement has `element`") &&
           expect(!node_kind_traits(NodeKind::CustomElement).has_component,
                  "CustomElement has no `component`") &&
           expect(node_kind_traits(NodeKind::Component).has_component,
                  "Component has `component`") &&
           expect(!node_kind_traits(NodeKind::Component).has_style,
                  "Component carries props, not style") &&
           expect(node_kind_traits(NodeKind::Component).takes_slot_children,
                  "Component accepts slot content") &&
           expect(!node_kind_traits(NodeKind::Component).is_leaf,
                  "Component may carry slot children, so it is not a leaf") &&
           expect(node_kind_traits(NodeKind::Text).has_text, "Text has `text`") &&
           expect(node_kind_traits(NodeKind::Text).is_inline_span, "Text may be an inline span");
}

bool check_leaves_cannot_have_children() {
    // The validator reports TESSERA_IR_LEAF from this flag alone, so the set must be exactly the
    // kinds whose section-3 shape is childless.
    bool ok = true;
    ok = expect(node_kind_traits(NodeKind::CustomElement).is_leaf, "CustomElement is a leaf") && ok;
    ok = expect(node_kind_traits(NodeKind::TextInput).is_leaf, "TextInput is a leaf") && ok;
    ok = expect(node_kind_traits(NodeKind::TextArea).is_leaf, "TextArea is a leaf") && ok;
    ok = expect(!node_kind_traits(NodeKind::View).is_leaf, "View may have children") && ok;
    ok = expect(!node_kind_traits(NodeKind::Scroll).is_leaf, "Scroll may have children") && ok;
    ok = expect(!node_kind_traits(NodeKind::Text).is_leaf, "Text may have inline span children") &&
         ok;
    return ok;
}

bool check_collections_are_the_two_virtual_kinds() {
    bool ok = true;
    for (std::size_t index = 0; index < kNodeKindCount; ++index) {
        const auto kind = static_cast<NodeKind>(index);
        const bool expected = kind == NodeKind::VirtualList || kind == NodeKind::VirtualGrid;
        ok = expect(node_kind_traits(kind).is_collection == expected,
                    "only VirtualList and VirtualGrid are collections") &&
             ok;
        // A collection is the only kind with an itemTemplate, and it must have one.
        ok = expect(node_kind_traits(kind).has_item_template == expected,
                    "an itemTemplate belongs to the collections alone") &&
             ok;
    }
    return ok;
}

bool check_a_corrupt_kind_reads_the_first_row() {
    // `node_kind_traits` is total: an out-of-range value reads View instead of past the array. A
    // corrupt enum is a bug, but reading past the table would be undefined behaviour.
    const auto beyond = static_cast<NodeKind>(kNodeKindCount + 5);
    return expect(node_kind_traits(beyond).name == std::string_view("View"),
                  "an out-of-range kind reads the first row, never past the table");
}

} // namespace

int main() {
    bool ok = true;
    ok = check_table_names_match_the_ir() && ok;
    ok = check_every_kind_is_reachable() && ok;
    ok = check_field_groups_match_the_validator() && ok;
    ok = check_leaves_cannot_have_children() && ok;
    ok = check_collections_are_the_two_virtual_kinds() && ok;
    ok = check_a_corrupt_kind_reads_the_first_row() && ok;
    if (!ok) {
        std::cerr << "node_kind_traits: FAILED\n";
        return 1;
    }
    std::cout << "node_kind_traits: all checks passed\n";
    return 0;
}
