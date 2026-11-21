// SPDX-License-Identifier: MIT
//
// The selector tree's reuse rule for AppHost::Impl (issue #711). Split out of host.cpp, which is at
// its size budget.

#include "host_impl.hpp"
#include "tessera/ui/scene/structure_epoch.hpp"

namespace tessera::app {

namespace {

// The topmost Style-dirty nodes: a dirty node restyles its whole subtree, so what lies below it is
// not visited again.
void restyle_dirty_subtrees(const ui::Node& node, ui::style::SceneStyleTree& tree) {
    if (ui::has_flag(node.dirty(), ui::Dirty::Style)) {
        tree.restyle_subtree(node);
        return;
    }
    for (const auto& child : node.children()) {
        if (ui::has_flag(child->subtree_dirty(), ui::Dirty::Style)) {
            restyle_dirty_subtrees(*child, tree);
        }
    }
}

} // namespace

bool AppHost::Impl::prepare_style_tree() {
    const bool reusable = style_tree_populated && tree != nullptr &&
                          tree->built_for_structure_epoch() == ui::scene_structure_epoch() &&
                          sheet.subtree_local();
    if (!reusable) {
        rebuild_style_tree();
        return false;
    }
    // The gate's "did this restyle move a box?" answer describes one round, so the record starts
    // empty here (issue #735, slice A).
    tree->clear_restyled();
    if (ui::has_flag(root.subtree_dirty(), ui::Dirty::Style)) {
        restyle_dirty_subtrees(root, *tree);
    }
    return true;
}

} // namespace tessera::app
