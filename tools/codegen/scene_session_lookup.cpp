// SPDX-License-Identifier: MIT

// The scene tree's node lookups (issue #171's `source.patch` slice split them out of
// `scene_session.cpp`, which was over the source-size limit).
//
// **Why they are their own unit.** Both are the same preorder walk over the live tree, one mutable
// and one const, and the pair is the only thing in the session that resolves an automation id to a
// node. Keeping them together is what makes "the const and mutable lookups agree" a property of one
// file rather than a coincidence across two.

#include "scene_session.hpp"

#include <cstdint>

namespace tessera::compiler::codegen::cli {
namespace {

namespace ui = tessera::ui;

} // namespace

const ui::Node* PreviewSession::find_by_automation_id(std::uint64_t automation_id) const {
    if (automation_id == 0 || root_ == nullptr) {
        return nullptr;
    }
    // The same preorder walk as `node_by_automation_id`, over a const tree: a hidden subtree is
    // skipped for the same reason (a hidden node is not clickable, so a script cannot reach one).
    struct Walk {
        static const ui::Node* find(const ui::Node& node, std::uint64_t id) {
            if (!node.rendered()) {
                return nullptr;
            }
            if (node.automation_id() == id) {
                return &node;
            }
            for (const auto& child : node.children()) {
                if (const ui::Node* found = find(*child, id)) {
                    return found;
                }
            }
            return nullptr;
        }
    };
    return Walk::find(*root_, automation_id);
}

ui::Node* PreviewSession::node_by_automation_id(std::uint64_t automation_id) {
    if (automation_id == 0) {
        return nullptr;
    }
    // Preorder search under the live tree, skipping hidden subtrees the way the harness's
    // `find_visible` does: a hidden node is not clickable, so a script cannot reach one.
    struct Walk {
        static ui::Node* find(ui::Node& node, std::uint64_t id) {
            if (!node.rendered()) {
                return nullptr;
            }
            if (node.automation_id() == id) {
                return &node;
            }
            for (const auto& child : node.children()) {
                if (ui::Node* found = find(*child, id)) {
                    return found;
                }
            }
            return nullptr;
        }
    };
    return Walk::find(*root_, automation_id);
}

ui::Node* PreviewSession::node_by_id(std::uint64_t node_id) {
    if (node_id == 0 || root_ == nullptr) {
        return nullptr;
    }
    // A scene node id, not an automation id: the build walk assigns ids in preorder, so this is the
    // identity a `ref` resolved to. Hidden subtrees are NOT skipped here, unlike the automation-id
    // walk: a behavior attaches to the node its ref names whether or not that node is currently
    // rendered, because a hidden node can become visible again without a rebuild.
    struct Walk {
        static ui::Node* find(ui::Node& node, std::uint64_t id) {
            if (node.id() == id) {
                return &node;
            }
            for (const auto& child : node.children()) {
                if (ui::Node* found = find(*child, id)) {
                    return found;
                }
            }
            return nullptr;
        }
    };
    return Walk::find(*root_, node_id);
}

} // namespace tessera::compiler::codegen::cli
