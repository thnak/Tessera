// SPDX-License-Identifier: MIT

#include "tessera/ui/style/scene_style.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace tessera::ui::style {

SceneStyleTree::SceneStyleTree(const Node& root) {
    build(root, nullptr);
}

void SceneStyleTree::build(const Node& node, StyleNode* parent) {
    Entry entry;
    entry.node = &node;
    entry.view.state = node.pseudo_state();
    entry.view.parent = parent;
    entries_.push_back(std::move(entry));

    Entry& stored = entries_.back();
    index_.emplace(&node, entries_.size() - 1);
    for (const auto& child : node.children()) {
        build(*child, &stored.view);
    }
}

StyleNode* SceneStyleTree::mutable_view(const Node& node) {
    const auto found = index_.find(&node);
    if (found == index_.end()) {
        return nullptr;
    }
    Entry& entry = entries_[found->second];
    entry.view.state = node.pseudo_state();
    return &entry.view;
}

const StyleNode* SceneStyleTree::view(const Node& node) {
    return mutable_view(node);
}

ComputedStyle SceneStyleTree::compute(const CompiledStyleSheet& sheet, const Node& node) {
    std::vector<const StyleNode*> chain;
    for (const Node* current = &node; current != nullptr; current = current->parent()) {
        StyleNode* view = mutable_view(*current);
        if (view == nullptr) {
            break;
        }
        chain.push_back(view);
    }
    if (chain.empty()) {
        return sheet.registry().initial_style();
    }
    std::reverse(chain.begin(), chain.end());

    ComputedStyle result;
    const ComputedStyle* parent = nullptr;
    for (const StyleNode* view : chain) {
        ComputedStyle next = sheet.compute(*view, parent);
        result = std::move(next);
        parent = &result;
    }
    return result;
}

} // namespace tessera::ui::style
