// SPDX-License-Identifier: MIT

// How the designer's override layer reaches the live scene tree (designer-protocol-v1 sections
// 9.2-9.4).
//
// **An override is an inline declaration.** The scene already has a style source that wins over the
// whole cascade and that every consumer reads through `SceneStyleTree::compute()` - layout, paint,
// the inspector, scroll sync - so applying an override through it means no consumer needs to know
// the layer exists, and the value the inspector reports is by construction the value being painted.
// The cost is the bounded inline table (16 declarations of 64/256 bytes), which `validate_override`
// checks up front so a value is refused rather than truncated.
//
// **Why every entry is put on and taken off together.** The layer is data the canvas session owns;
// this unit is the only place that turns it into node state. A single `resync` that takes every
// applied override off and puts the layer's current entries on is the one operation `set`, `clear`,
// `revert`, `undo` and `redo` all need, so none of them has a path of its own that could leave a
// stale value on a node.

#include "scene_session.hpp"

#include <cstdint>
#include <string>

namespace tessera::compiler::codegen::cli {
namespace {

namespace ui = tessera::ui;

// The node with this automation id anywhere in the tree, hidden subtrees included: an override set
// on a node a conditional currently hides must still apply when the condition flips.
ui::Node* find_any(ui::Node& node, std::uint64_t automation_id) {
    if (node.automation_id() == automation_id) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (ui::Node* found = find_any(*child, automation_id)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

void PreviewSession::apply_overrides() {
    if (options_.overrides == nullptr || root_ == nullptr) {
        return;
    }
    for (const StyleOverride& entry : options_.overrides->entries()) {
        ui::Node* const node = find_any(*root_, entry.automation_id);
        if (node == nullptr) {
            // The node is not in this tree (an edit removed it); the entry stays in the layer.
            continue;
        }
        AppliedOverride applied;
        applied.automation_id = entry.automation_id;
        // The property and value outlive the node's borrow: they are appended to a deque and never
        // moved until the tree they were applied to is gone.
        override_text_.push_back(entry.property);
        applied.property = override_text_.back();
        for (const ui::Node::InlineStyleDeclaration& existing : node->inline_style()) {
            if (existing.name == entry.property) {
                applied.had_original = true;
                applied.original = existing.value;
                break;
            }
        }
        override_text_.push_back(entry.value);
        if (node->set_inline_style(applied.property, override_text_.back())) {
            applied_overrides_.push_back(applied);
        }
    }
}

void PreviewSession::restore_overrides() {
    if (root_ == nullptr) {
        applied_overrides_.clear();
        return;
    }
    for (const AppliedOverride& applied : applied_overrides_) {
        ui::Node* const node = find_any(*root_, applied.automation_id);
        if (node == nullptr) {
            continue;
        }
        if (applied.had_original) {
            (void)node->set_inline_style(applied.property, applied.original);
        } else {
            (void)node->remove_inline_style(applied.property);
        }
    }
    applied_overrides_.clear();
}

bool PreviewSession::resync_overrides() {
    if (!built_) {
        return false;
    }
    // The old strings are released only after every node has let go of them.
    restore_overrides();
    override_text_.clear();
    apply_overrides();
    return relayout();
}

} // namespace tessera::compiler::codegen::cli
