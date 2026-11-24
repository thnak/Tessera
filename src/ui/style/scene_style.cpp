// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/layout_digest.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::style {

SceneStyleTree::SceneStyleTree(const Node& root)
    : structure_epoch_(scene_structure_epoch()) {
    build(root, nullptr);
}

// Fills `entry`'s selector view from the scene node. Run again on a node whose style inputs changed
// (`restyle_subtree`), so it must leave nothing from the previous contents behind.
void SceneStyleTree::fill_view(Entry& entry, const Node& node, StyleNode* parent) {
    entry.node = &node;
    StyleNode& view = entry.view;
    // A component instance host is typed by its component name, so the host side of `::part()`
    // can name it (`Button::part(label)`, stylesheet-v1 section 6.3).
    const std::string_view component = node.component_name();
    view.component_host = !component.empty();
    view.tag = view.component_host ? component : node.element_kind();
    view.alias_tag = node.element_tag();
    view.id = node.element_id();
    view.part = node.part();
    view.state = node.pseudo_state();
    // The `::part()` boundary is the nearest ancestor component instance; precomputed so matching
    // never walks ancestors for it.
    view.host = parent == nullptr ? nullptr : (parent->component_host ? parent : parent->host);
    const auto& paint = node.paint_data();
    view.has_text = paint.has_value() && paint->kind == tessera::ui::PaintKind::Text;

    // `role` and `aria-*` are the attributes a selector can name, as in a browser (`[role="list"]`,
    // `[aria-level="2"]`). The user-agent sheet needs them: `h1`-`h6` are all `Text` and differ
    // only by `aria-level` (issue #606). The views point at node-owned storage, which outlives this
    // tree.
    view.attributes.clear();
    if (!node.role().empty()) {
        view.attributes.push_back(AttributeView{"role", node.role()});
    }
    for (const Node::AriaAttribute& aria : node.aria_attributes()) {
        view.attributes.push_back(AttributeView{aria.name, aria.value});
    }

    // The class views point into this entry's own token storage. The storage is filled completely
    // before any view is taken, and a vector move keeps its element addresses.
    std::vector<std::string> tokens;
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
            tokens.emplace_back(class_name.substr(end, position - end));
        }
    }
    entry.class_tokens = std::move(tokens);
    view.classes.clear();
    for (const std::string& token : entry.class_tokens) {
        view.classes.push_back(token);
    }
    view.parent = parent;
}

void SceneStyleTree::build(const Node& node, StyleNode* parent) {
    build_node(node, parent);
    // The memo is a dense array parallel to `entries_` (issue #735, slice G), and `resolve` hands
    // out references into it. Sizing it here, once, is what keeps those references valid for the
    // tree's lifetime: a `std::vector` that grew later would move every element it already holds.
    // `build_node` has just filled `entries_`, so this is the one moment the node count is known.
    // Nothing resolves during the build, so growing the arrays while it ran would have been safe
    // too - but reserving once is what makes the "reference stays valid" contract provable.
    memo_.resize(entries_.size());
    memoised_.assign(entries_.size(), std::uint8_t{0});
}

void SceneStyleTree::build_node(const Node& node, StyleNode* parent) {
    entries_.emplace_back();
    Entry& stored = entries_.back();
    index_.emplace(&node, entries_.size() - 1);
    slot_nodes_.push_back(&node);
    fill_view(stored, node, parent);
    for (const auto& child : node.children()) {
        build_node(*child, &stored.view);
    }
}

void SceneStyleTree::restyle_subtree(const Node& node) {
    const auto found = index_.find(&node);
    if (found == index_.end()) {
        return;
    }
    // The topmost restyled node: a node's style depends only on itself and its ancestors, so
    // everything below this one is recomputed and everything outside it kept its style.
    restyled_roots_.push_back(&node);
    const auto parent = node.parent() == nullptr ? index_.end() : index_.find(node.parent());
    refresh(node, parent == index_.end() ? nullptr : &entries_[parent->second].view);
}

void SceneStyleTree::refresh(const Node& node, StyleNode* parent) {
    const auto found = index_.find(&node);
    if (found == index_.end()) {
        return;
    }
    const std::size_t slot = found->second;
    Entry& entry = entries_[slot];
    fill_view(entry, node, parent);
    // Remember the layout digest this node's style had before the restyle, so the caller can ask
    // whether the restyle moved a box (issue #735, slice A). A node with no memoised style has no
    // "before" to compare against, so it is recorded as changed.
    const bool had_style = memoised_[slot] != 0;
    restyled_.push_back(
        RestyledEntry{&node, had_style ? memo_[slot].cached_digest() : std::uint64_t{0}});
    // Drop the style itself, not just the flag: `memo_.erase` used to release the entry, and a
    // dropped generation should not keep its shared entries alive through a restyle.
    memo_[slot] = ComputedStyle{};
    memoised_[slot] = 0;
    for (const auto& child : node.children()) {
        refresh(*child, &entry.view);
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

// The memoised container chain of one node, presented as a `ContainerSizeSource` so a `@container`
// rule resolves without walking the ancestors again (issue #693). The chain is nearest last, the
// same order `AncestorContainers` builds, so both paths answer identically.
class ChainContainers final : public ContainerSizeSource {
public:
    ChainContainers(const std::vector<SceneStyleTree::ContainerRef>& chain,
                    SceneStyleTree::ContainerSizeFn size, void* context)
        : chain_(chain),
          size_(size),
          context_(context) {}

    bool find(const StyleNode&, std::string_view name, ContainerSize& out) const override {
        if (size_ == nullptr) {
            return false;
        }
        for (std::size_t i = chain_.size(); i-- > 0;) {
            const SceneStyleTree::ContainerRef& container = chain_[i];
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

    const std::vector<SceneStyleTree::ContainerRef>& chain_;
    SceneStyleTree::ContainerSizeFn size_;
    void* context_;
};

} // namespace

namespace {

// Inline declarations win over the cascade (Origin::Inline). They are already bounded and borrowed
// on the node; the registry expands shorthands into longhands. They may define or override custom
// properties on this node, so substitution runs again with the parent's computed style as the
// inheritance fallback. Call it before a node's style becomes its children's parent.
void apply_inline_declarations(const CompiledStyleSheet& sheet, const Node& node,
                               ComputedStyle& result, const ComputedStyle* parent) {
    if (node.inline_style().empty()) {
        return;
    }
    for (const Node::InlineStyleDeclaration& declaration : node.inline_style()) {
        Declaration parsed{std::string(declaration.name), std::string(declaration.value), false};
        const PropertyExpansionResult expanded = sheet.registry().expand(parsed);
        for (const Declaration& longhand : expanded.declarations) {
            result.set(longhand.property, longhand.value);
        }
    }
    resolve_variables(result, parent);
    resolve_font_size(result, parent);
}

} // namespace

void SceneStyleTree::begin_memo(bool keep_entries) {
    memo_active_ = true;
    if (!keep_entries) {
        // Every slot is dropped at once: the flag array is the whole "has a memoised style" state,
        // so clearing it is one fill instead of a walk over the styles. The styles themselves are
        // released too, so a dropped generation does not keep its shared entries alive; that is a
        // per-generation cost, not a per-frame one (a new generation rebuilds the tree anyway).
        memoised_.assign(memoised_.size(), std::uint8_t{0});
        std::fill(memo_.begin(), memo_.end(), ComputedStyle{});
        // The chains are dropped with the styles they belong to; a chain is only meaningful for the
        // style generation that produced it.
        containers_.clear();
        outside_style_.reset();
    }
}

void SceneStyleTree::end_memo() {
    // The entries are the tree's computed styles for this generation; keep them for the paint pass.
}

void SceneStyleTree::set_container_source(ContainerSizeFn size, void* context) {
    // A host calls this every frame (the settle loop installs or clears the source as the sheet
    // gains or loses `@container` rules), so an unchanged source must not drop the memo: doing so
    // would make every frame a cold style generation and defeat the layout-unchanged gate.
    if (size == container_size_ && context == container_context_) {
        return;
    }
    container_size_ = size;
    container_context_ = context;
    // The chain a memoised style was computed under is only valid for the source that produced it,
    // so a new source starts a new generation.
    memoised_.assign(memoised_.size(), std::uint8_t{0});
    std::fill(memo_.begin(), memo_.end(), ComputedStyle{});
    containers_.clear();
    outside_style_.reset();
}

void SceneStyleTree::append_container(std::vector<ContainerRef>& chain, const Node& node,
                                      const ComputedStyle& computed) {
    const std::string* type = computed.find("container-type");
    if (type == nullptr || (*type != "size" && *type != "inline-size")) {
        return;
    }
    const std::string* name = computed.find("container-name");
    chain.push_back(ContainerRef{&node, *type == "size",
                                 name != nullptr && *name != "none" ? *name : std::string{}});
}

std::size_t SceneStyleTree::slot_of(const Node& node) const {
    if (cursor_ < slot_nodes_.size() && slot_nodes_[cursor_] == &node) {
        return cursor_;
    }
    const auto found = index_.find(&node);
    return found == index_.end() ? kNoSlot : found->second;
}

const ComputedStyle& SceneStyleTree::resolve(const CompiledStyleSheet& sheet, const Node& node) {
    const std::size_t slot = slot_of(node);
    if (slot == kNoSlot) {
        // Outside the captured tree: the registry's initial style, computed once per generation.
        if (!outside_style_.has_value()) {
            outside_style_.emplace(sheet.registry().initial_style());
        }
        return *outside_style_;
    }
    cursor_ = slot + 1; // the next node of a pre-order walk
    if (memoised_[slot] != 0) {
        return memo_[slot];
    }
    // The scene stays the source of truth for interaction state, so the view is refreshed from the
    // node before it is matched (what `mutable_view` does; the index lookup is already done here).
    entries_[slot].view.state = node.pseudo_state();
    // The chain stops at the first ancestor outside the captured tree, as the un-memoised walk
    // does.
    const Node* parent_node = node.parent();
    const bool has_parent = parent_node != nullptr && index_.contains(parent_node);
    const ComputedStyle* parent = has_parent ? &resolve(sheet, *parent_node) : nullptr;
    // A `@container` rule resolves against the nearest ancestor container, never the node itself,
    // so the chain handed to the cascade is the parent's (issue #693). The chain is only built for
    // a sheet that actually has container queries, so a sheet without them pays nothing.
    const bool use_containers = container_size_ != nullptr && sheet.has_container_queries();
    const std::vector<ContainerRef>* parent_chain = nullptr;
    if (use_containers) {
        if (containers_.size() < memo_.size()) {
            containers_.resize(memo_.size());
        }
        if (has_parent) {
            parent_chain = &containers_[index_.find(parent_node)->second];
        }
    }
    const std::vector<ContainerRef> empty;
    ChainContainers containers(parent_chain != nullptr ? *parent_chain : empty, container_size_,
                               container_context_);
    ComputedStyle computed =
        sheet.compute(entries_[slot].view, parent, use_containers ? &containers : nullptr);
    apply_inline_declarations(sheet, node, computed, parent);
    // The slot already exists (the array was sized at the end of `build`), so this assignment never
    // reallocates and the reference below stays valid for the generation.
    memo_[slot] = std::move(computed);
    memoised_[slot] = 1;
    cursor_ = slot + 1; // resolving the parent above moved it
    if (use_containers) {
        // This node's chain is its parent's plus this node when this node is a query container, so
        // a child's `@container` lookup is a scan of a short vector instead of an ancestor walk.
        std::vector<ContainerRef>& chain = containers_[slot];
        chain.clear();
        if (parent_chain != nullptr) {
            chain = *parent_chain;
        }
        append_container(chain, node, memo_[slot]);
    }
    return memo_[slot];
}

bool SceneStyleTree::restyled_layout_changed(const CompiledStyleSheet& sheet) {
    // One answer per restyle round: the record is consumed here, so a frame that declines cannot
    // leave its entries behind for the next frame to re-detect (which would make every frame after
    // a real layout change decline too, and the gate would never accept again).
    std::vector<RestyledEntry> restyled;
    restyled.swap(restyled_);
    for (const RestyledEntry& entry : restyled) {
        // No "before" digest: the node had no memoised style, so nothing proves the restyle kept
        // its layout inputs. Treat it as changed rather than guess.
        if (entry.digest == 0) {
            return true;
        }
        // Resolving recomputes exactly the nodes `restyle_subtree` dropped, from their parent's
        // memoised style, so this is the same work the layout input would have done - but only for
        // the restyled subtree, not the whole tree.
        const ComputedStyle& after = resolve(sheet, *entry.node);
        if (layout_digest(after) != entry.digest) {
            return true;
        }
    }
    return false;
}

ComputedStyle SceneStyleTree::compute(const CompiledStyleSheet& sheet, const Node& node,
                                      ContainerSizeFn container_size, void* container_context) {
    if (memo_active_ && container_size == nullptr) {
        return resolve(sheet, node);
    }
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
    // Keep every computed style of the chain, inline declarations included: a child inherits its
    // parent's *computed* value (issue #719), and the parent value is also what resolves a `var()`
    // in an inline declaration that ends up invalid at computed-value time.
    std::vector<ComputedStyle> styles;
    styles.reserve(chain.size());
    for (std::size_t i = 0; i < chain.size(); ++i) {
        const ComputedStyle* parent = i == 0 ? nullptr : &styles.back();
        styles.push_back(sheet.compute(*chain[i], parent, use_containers ? &containers : nullptr));
        apply_inline_declarations(sheet, *nodes[i], styles.back(), parent);
        if (use_containers && i + 1 < chain.size()) {
            containers.push(*nodes[i], styles.back());
        }
    }
    return std::move(styles.back());
}

} // namespace tessera::ui::style
