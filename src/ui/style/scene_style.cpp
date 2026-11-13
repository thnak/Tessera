// SPDX-License-Identifier: MIT

#include "tessera/ui/style/scene_style.hpp"

#include <algorithm>
#include <cctype>
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
    // A component instance host is typed by its component name, so the host side of `::part()`
    // can name it (`Button::part(label)`, stylesheet-v1 section 6.3).
    const std::string_view component = node.component_name();
    entry.view.component_host = !component.empty();
    entry.view.tag = entry.view.component_host ? component : node.element_kind();
    entry.view.id = node.element_id();
    entry.view.part = node.part();
    entry.view.state = node.pseudo_state();
    // The `::part()` boundary is the nearest ancestor component instance; precomputed so matching
    // never walks ancestors for it.
    entry.view.host =
        parent == nullptr ? nullptr : (parent->component_host ? parent : parent->host);
    const auto& paint = node.paint_data();
    entry.view.has_text = paint.has_value() && paint->kind == tessera::ui::PaintKind::Text;

    const std::string_view class_name = node.class_name();
    std::size_t position = 0;
    while (position < class_name.size()) {
        while (position < class_name.size() &&
               std::isspace(static_cast<unsigned char>(class_name[position])) != 0) {
            ++position;
        }
        const std::size_t end = position;
        while (position < class_name.size() &&
               std::isspace(static_cast<unsigned char>(class_name[position])) == 0) {
            ++position;
        }
        if (position > end) {
            class_storage_.emplace_back(class_name.substr(end, position - end));
            entry.view.classes.push_back(class_storage_.back());
        }
    }

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
    // Keep every computed style of the chain: the node's parent value is needed to resolve a
    // `var()` in an inline declaration that ends up invalid at computed-value time.
    std::vector<ComputedStyle> styles;
    styles.reserve(chain.size());
    for (std::size_t i = 0; i < chain.size(); ++i) {
        const ComputedStyle* parent = i == 0 ? nullptr : &styles.back();
        styles.push_back(sheet.compute(*chain[i], parent, use_containers ? &containers : nullptr));
        if (use_containers && i + 1 < chain.size()) {
            containers.push(*nodes[i], styles.back());
        }
    }
    // Inline declarations win over the cascade (Origin::Inline). They are already bounded and
    // borrowed on the node; the registry expands shorthands into longhands.
    ComputedStyle result = std::move(styles.back());
    const bool has_inline = !node.inline_style().empty();
    for (const Node::InlineStyleDeclaration& declaration : node.inline_style()) {
        Declaration parsed{std::string(declaration.name), std::string(declaration.value), false};
        const PropertyExpansionResult expanded = sheet.registry().expand(parsed);
        for (const Declaration& longhand : expanded.declarations) {
            result.set(longhand.property, longhand.value);
        }
    }
    if (has_inline) {
        // Inline declarations may define or override custom properties on this node, so the
        // substitution runs again with the parent style as the inheritance fallback.
        const ComputedStyle* parent = styles.size() > 1 ? &styles[styles.size() - 2] : nullptr;
        resolve_variables(result, parent);
    }
    return result;
}

} // namespace tessera::ui::style
