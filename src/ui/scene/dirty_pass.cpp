// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/dirty_pass.hpp"

#include <stdexcept>

namespace tessera::ui {

DirtyPass::DirtyPass(std::size_t max_nodes)
    : max_nodes_(max_nodes) {
    if (max_nodes_ == 0) {
        throw std::invalid_argument("dirty pass max_nodes must be greater than zero");
    }
    nodes_.reserve(max_nodes_);
}

void DirtyPass::collect(Node& root, Dirty domain) {
    reset();
    if (domain != Dirty::None) {
        visit(root, domain);
    }
}

void DirtyPass::clear(Node& root, Dirty domain) {
    reset();
    if (domain != Dirty::None) {
        clear_visit(root, domain);
    }
}

void DirtyPass::reset() {
    nodes_.clear();
    overflowed_ = false;
}

void DirtyPass::visit(Node& node, Dirty domain) {
    if (!has_any(node.subtree_dirty(), domain)) {
        return;
    }
    if (has_any(node.dirty(), domain)) {
        if (nodes_.size() >= max_nodes_) {
            overflowed_ = true;
            return;
        }
        nodes_.push_back(DirtyNode{&node, node.dirty() & domain});
    }
    for (const auto& child : node.children()) {
        visit(*child, domain);
    }
}

void DirtyPass::clear_visit(Node& node, Dirty domain) {
    if (!has_any(node.subtree_dirty(), domain)) {
        return;
    }
    node.clear_dirty(domain);
    for (const auto& child : node.children()) {
        clear_visit(*child, domain);
    }
}

} // namespace tessera::ui
