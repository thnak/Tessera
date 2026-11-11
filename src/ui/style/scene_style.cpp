// SPDX-License-Identifier: MIT

#include "tessera/ui/style/scene_style.hpp"

#include <algorithm>
#include <string>
#include <string_view>
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

namespace {

// The query containers among the ancestors resolved so far, nearest last.
class AncestorContainers final : public ContainerSizeSource {
public:
    AncestorContainers(SceneStyleTree::ContainerSizeFn size, void* context)
        : size_(size),
          context_(context) {}

    void push(const Node& node, const ComputedStyle& computed) {
        const std::string* type = computed.find("container-type");
        if (type == nullptr || (*type != "size" && *type != "inline-size")) {
            return;
        }
        const std::string* name = computed.find("container-name");
        containers_.push_back(Container{
            &node, *type == "size", name != nullptr && *name != "none" ? *name : std::string{}});
    }

    bool find(const StyleNode&, std::string_view name, ContainerSize& out) const override {
        if (size_ == nullptr) {
            return false;
        }
        for (std::size_t i = containers_.size(); i-- > 0;) {
            const Container& container = containers_[i];
            if (!name.empty() && !has_name(container.names, name)) {
                continue;
            }
            if (!size_(context_, *container.node, out)) {
                return false;
            }
            out.height_queryable = container.block_axis;
            return true;
        }
        return false;
    }

private:
    struct Container {
        const Node* node = nullptr;
        bool block_axis = false;
        std::string names; // whitespace-separated `container-name` list
    };

    static bool has_name(const std::string& names, std::string_view wanted) {
        std::size_t pos = 0;
        while (pos < names.size()) {
            const std::size_t end = names.find(' ', pos);
            const std::size_t stop = end == std::string::npos ? names.size() : end;
            if (std::string_view(names).substr(pos, stop - pos) == wanted) {
                return true;
            }
            pos = stop + 1;
        }
        return false;
    }

    SceneStyleTree::ContainerSizeFn size_;
    void* context_;
    std::vector<Container> containers_;
};

} // namespace

ComputedStyle SceneStyleTree::compute(const CompiledStyleSheet& sheet, const Node& node,
                                      ContainerSizeFn container_size, void* container_context) {
    std::vector<const StyleNode*> chain;
    std::vector<const Node*> nodes;
    for (const Node* current = &node; current != nullptr; current = current->parent()) {
        StyleNode* view = mutable_view(*current);
        if (view == nullptr) {
            break;
        }
        chain.push_back(view);
        nodes.push_back(current);
    }
    if (chain.empty()) {
        return sheet.registry().initial_style();
    }
    std::reverse(chain.begin(), chain.end());
    std::reverse(nodes.begin(), nodes.end());

    AncestorContainers containers(container_size, container_context);
    const bool use_containers = container_size != nullptr && sheet.has_container_queries();
    ComputedStyle result;
    const ComputedStyle* parent = nullptr;
    for (std::size_t i = 0; i < chain.size(); ++i) {
        ComputedStyle next =
            sheet.compute(*chain[i], parent, use_containers ? &containers : nullptr);
        result = std::move(next);
        parent = &result;
        if (use_containers && i + 1 < chain.size()) {
            containers.push(*nodes[i], result);
        }
    }
    // Inline declarations win over the cascade (Origin::Inline). They are already bounded and
    // borrowed on the node; the registry expands shorthands into longhands.
    for (const Node::InlineStyleDeclaration& declaration : node.inline_style()) {
        Declaration parsed{std::string(declaration.name), std::string(declaration.value), false};
        const PropertyExpansionResult expanded = sheet.registry().expand(parsed);
        for (const Declaration& longhand : expanded.declarations) {
            result.set(longhand.property, longhand.value);
        }
    }
    return result;
}

} // namespace tessera::ui::style
