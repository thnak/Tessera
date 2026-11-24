// SPDX-License-Identifier: MIT
//
// The retained Yoga tree (issue #735, slice D): one `YGNode` per node id, kept across frames. A
// node's style is rebuilt only when its layout digest changed, and Yoga propagates the dirtiness
// from there, so a size change re-applies one node's style instead of every node's.
//
// The tree owns everything a measure callback reads - the text, its resolved `text::TextStyle`, the
// padding the text-fit fact uses, and a grid container's whole subtree input - and the callback
// context is the `RetainedNode` itself. A callback therefore cannot outlive its storage: the
// `RetainedNode` lives in `RetainedTreeState::nodes` exactly as long as the `YGNode` that points at
// it, and both are destroyed together.

#include "layout_cold.hpp"
#include "layout_measure.hpp"
#include "layout_retained.hpp"
#include "layout_style.hpp"
#include "tessera/ui/layout/layout_signature.hpp"
#include "tessera/ui/style/layout_digest.hpp"
#include "tessera/ui/text/text_measure.hpp"
#include "tessera/ui/text/text_measure_cache.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include <yoga/Yoga.h>

namespace tessera::ui::layout::detail {
namespace {

using detail::apply_block_auto_margin_fill;
using detail::apply_block_child_style;
using detail::apply_root_min_height;
using detail::apply_style;
using detail::is_keyword;
using detail::order_value;
using detail::paint_padding_px;
using detail::root_auto_margin_offset;
using detail::with_element_font_size;

// The measure callbacks. Their context is the `RetainedNode` itself, so what a callback reads lives
// exactly as long as the Yoga node that points at it: no per-frame context object, and nothing that
// can dangle when the `LayoutInput` tree dies with the call.
YGSize measure_text(YGNodeConstRef node, float width, YGMeasureMode width_mode, float height,
                    YGMeasureMode height_mode) {
    auto* retained = static_cast<RetainedNode*>(YGNodeGetContext(node));
    if (retained == nullptr || retained->owner == nullptr || retained->owner->cache == nullptr) {
        return YGSize{0.0f, 0.0f};
    }
    return measure_text_box(retained->text, retained->text_style, *retained->owner->cache, width,
                            width_mode, height, height_mode);
}

YGSize measure_grid(YGNodeConstRef node, float width, YGMeasureMode width_mode, float height,
                    YGMeasureMode height_mode) {
    auto* retained = static_cast<RetainedNode*>(YGNodeGetContext(node));
    if (retained == nullptr || retained->owner == nullptr || retained->grid_input == nullptr) {
        return YGSize{0.0f, 0.0f};
    }
    return measure_grid_box(*retained->grid_input, retained->owner->viewport,
                            *retained->owner->cache, width, width_mode, height, height_mode);
}

// Drops `retained`'s subtree from the id map. The Yoga nodes are already gone, so this only
// releases the `RetainedNode`s; the children are erased before the parent because the parent owns
// the vector the walk is reading.
void forget_subtree(RetainedTreeState& state, RetainedNode* retained) {
    for (RetainedNode* child : retained->children) {
        forget_subtree(state, child);
    }
    state.nodes.erase(retained->id);
}

void destroy_subtree(RetainedTreeState& state, RetainedNode* retained) {
    YGNodeFreeRecursive(retained->node);
    forget_subtree(state, retained);
}

// Discards the whole tree. Every node is either attached to an owner or is the root of a detached
// fragment (a pass that restarted mid-sync can leave one), so the ownerless nodes are collected
// first and each fragment is then freed exactly once. Collecting before freeing matters: freeing a
// fragment root deletes its descendants' Yoga nodes, so reading an owner afterwards would be a
// use-after-free.
void reset(RetainedTreeState& state) {
    std::vector<YGNodeRef> fragments;
    for (auto& entry : state.nodes) {
        if (YGNodeGetOwner(entry.second->node) == nullptr) {
            fragments.push_back(entry.second->node);
        }
    }
    for (YGNodeRef fragment : fragments) {
        YGNodeFreeRecursive(fragment);
    }
    state.nodes.clear();
    state.root = nullptr;
    state.primed = false;
}

// Makes the Yoga child list match `retained.desired`, which is in layout order. Insert and remove
// rather than `YGNodeSetChildren`, because only those maintain Yoga's `contentsChildrenCount_` -
// the counter that decides whether `display: contents` children have to be skipped - and a
// stale-low count would make Yoga lay out a `display: contents` node as a real box.
void reconcile_children(RetainedTreeState& state, RetainedNode& retained) {
    YGNodeRef node = retained.node;
    // A steady frame keeps every child in place. Checking that first is one pass; the removal scan
    // below searches `desired` for every existing child, which is quadratic in a container with
    // thousands of children (a 2.3k-row list spent ~4 percent of the incremental frame there).
    if (retained.children == retained.desired) {
        return;
    }
    for (std::size_t i = retained.children.size(); i-- > 0;) {
        RetainedNode* child = retained.children[i];
        if (std::find(retained.desired.begin(), retained.desired.end(), child) !=
            retained.desired.end()) {
            continue;
        }
        YGNodeRemoveChild(node, child->node);
        retained.children.erase(retained.children.begin() + static_cast<std::ptrdiff_t>(i));
        destroy_subtree(state, child);
    }
    for (std::size_t i = 0; i < retained.desired.size(); ++i) {
        RetainedNode* want = retained.desired[i];
        if (i < retained.children.size() && retained.children[i] == want) {
            continue;
        }
        const auto found = std::find(retained.children.begin() + static_cast<std::ptrdiff_t>(i),
                                     retained.children.end(), want);
        if (found != retained.children.end()) {
            YGNodeRemoveChild(node, want->node);
            retained.children.erase(found);
        } else if (YGNodeGetOwner(want->node) != nullptr) {
            // The node moved to a different parent. Reconciling that in place would need a second
            // pass over the whole tree, so the pass is discarded and one from-scratch pass runs.
            state.restart = true;
            return;
        }
        YGNodeInsertChild(node, want->node, i);
        retained.children.insert(retained.children.begin() + static_cast<std::ptrdiff_t>(i), want);
    }
}

// Switches how `retained` is measured. Yoga forbids a measure function on a node with children and
// children on a node with a measure function, so a container that becomes a leaf drops its subtree
// first and a leaf that becomes a container clears its measure function first.
void set_measure(RetainedTreeState& state, RetainedNode& retained, RetainedMeasure next) {
    if (next == RetainedMeasure::None) {
        YGNodeSetMeasureFunc(retained.node, nullptr);
        YGNodeSetContext(retained.node, nullptr);
    } else {
        for (RetainedNode* child : retained.children) {
            destroy_subtree(state, child);
        }
        retained.children.clear();
        YGNodeSetContext(retained.node, &retained);
        YGNodeSetMeasureFunc(retained.node,
                             next == RetainedMeasure::Grid ? measure_grid : measure_text);
    }
    retained.measure = next;
}

// Re-applies the style of a node whose layout digest changed, and refreshes every fact the frame
// path reads off it. Rebuilding from a pristine style rather than patching in place is what keeps
// the retained tree equal to the from-scratch one: `apply_style` only writes the longhands the
// style *has*, so a longhand the author just removed would otherwise keep its old Yoga value.
void apply_node_style(RetainedTreeState& state, RetainedNode& retained, const LayoutInput& input,
                      bool block_parent) {
    YGNodeCopyStyle(retained.node, state.pristine);
    apply_style(retained.node, input.style, with_element_font_size(state.viewport, input.style));
    if (block_parent) {
        apply_block_child_style(retained.node); // `margin: 0 auto` fill, no shrink (issue #611)
    }
    retained.style_digest = style::layout_digest(input.style);
    retained.fit_valid = false; // text style, padding and `display` all feed the text-fit fact
    retained.block_parent = block_parent;
    retained.grid = is_grid_container(input.style);
    retained.display_none = is_keyword(input.style.find("display"), "none");
    retained.block = is_keyword(input.style.find("display"), "block");
    retained.rtl = is_keyword(input.style.find("direction"), "rtl");
    retained.sizes_to_content = is_keyword(input.style.find("field-sizing"), "content");
    retained.order = order_value(input.style);
    retained.text_style = text::text_style_from(input.style);
    retained.pad_left = paint_padding_px(input.style, "padding-left");
    retained.pad_top = paint_padding_px(input.style, "padding-top");
    retained.pad_right = paint_padding_px(input.style, "padding-right");
    retained.pad_bottom = paint_padding_px(input.style, "padding-bottom");
    ++state.style_applications;
}

RetainedNode* sync_node(RetainedTreeState& state, const LayoutInput& input, bool block_parent);

// The container half of `sync_node`: sync every child, order them by CSS `order`, and reconcile.
void sync_children(RetainedTreeState& state, RetainedNode& retained, const LayoutInput& input) {
    retained.desired.clear();
    for (const LayoutInput& child : input.children) {
        if (RetainedNode* synced = sync_node(state, child, retained.block)) {
            retained.desired.push_back(synced);
        }
        if (state.restart) {
            return;
        }
    }
    // CSS `order` (stylesheet-v1 section 11, issue #213): ascending `order`, ties keeping source
    // order. A stable insertion sort, in place: `std::stable_sort` would allocate a scratch buffer
    // on the frame path, and a container's child count is small.
    for (std::size_t i = 1; i < retained.desired.size(); ++i) {
        RetainedNode* key = retained.desired[i];
        std::size_t j = i;
        while (j > 0 && retained.desired[j - 1]->order > key->order) {
            retained.desired[j] = retained.desired[j - 1];
            --j;
        }
        retained.desired[j] = key;
    }
    reconcile_children(state, retained);
}

RetainedNode* sync_node(RetainedTreeState& state, const LayoutInput& input, bool block_parent) {
    if (state.restart) {
        return nullptr;
    }
    RetainedNode* retained = nullptr;
    const auto found = state.nodes.find(input.id);
    if (found != state.nodes.end()) {
        retained = found->second.get();
        if (retained->visited == state.pass) {
            // Two input nodes share an id, so the id -> node map cannot represent this tree. The
            // pass is discarded rather than producing boxes for the wrong node.
            state.restart = true;
            return nullptr;
        }
    } else {
        auto created = std::make_unique<RetainedNode>();
        created->node = YGNodeNewWithConfig(unrounded_config());
        created->id = input.id;
        created->owner = &state;
        retained = created.get();
        state.nodes.emplace(input.id, std::move(created));
    }
    retained->visited = state.pass;

    if (retained->style_digest != style::layout_digest(input.style) ||
        retained->block_parent != block_parent) {
        apply_node_style(state, *retained, input, block_parent);
    }

    bool text_changed = false;
    if (retained->text != input.text) {
        retained->text = input.text;
        text_changed = true;
    }
    if (retained->text_wrap != input.text_wrap) {
        retained->text_wrap = input.text_wrap;
        text_changed = true;
    }
    if (text_changed) {
        retained->fit_valid = false;
    }

    // How this node is measured, mirroring `build_tree`: a grid container is a measured leaf over
    // its own subtree, a node with text (or `field-sizing: content`) and no children is a measured
    // text leaf, and everything else is a container.
    RetainedMeasure next = RetainedMeasure::None;
    if (retained->grid) {
        next = RetainedMeasure::Grid;
    } else if ((!input.text.empty() || retained->sizes_to_content) && input.children.empty()) {
        next = RetainedMeasure::Text;
    }
    if (next != retained->measure) {
        set_measure(state, *retained, next);
    }

    if (retained->measure == RetainedMeasure::Grid) {
        // The grid engine walks the whole subtree, so the tree owns that subtree's input. Its
        // signature covers every longhand and every text the engine can read, so an unchanged
        // signature means the owned copy is still exact and nothing has to be re-measured.
        const std::uint64_t signature = layout_signature(input, state.viewport);
        if (retained->grid_input == nullptr || retained->grid_signature != signature) {
            if (retained->grid_input == nullptr) {
                retained->grid_input = std::make_unique<LayoutInput>();
            }
            *retained->grid_input = input;
            retained->grid_signature = signature;
            YGNodeMarkDirty(retained->node);
        }
        return retained;
    }
    if (retained->measure == RetainedMeasure::Text) {
        if (text_changed) {
            YGNodeMarkDirty(retained->node);
        }
        return retained;
    }

    sync_children(state, *retained, input);
    return retained;
}

} // namespace

RetainedTree::RetainedTree()
    : state_(std::make_unique<RetainedTreeState>()) {
    state_->pristine = YGNodeNewWithConfig(unrounded_config());
}

RetainedTree::~RetainedTree() {
    if (state_ == nullptr) {
        return;
    }
    reset(*state_);
    if (state_->pristine != nullptr) {
        YGNodeFree(state_->pristine);
    }
}

RetainedTree::RetainedTree(RetainedTree&&) noexcept = default;

RetainedTree& RetainedTree::operator=(RetainedTree&& other) noexcept {
    if (this != &other) {
        if (state_ != nullptr) {
            reset(*state_);
            if (state_->pristine != nullptr) {
                YGNodeFree(state_->pristine);
            }
        }
        state_ = std::move(other.state_);
    }
    return *this;
}

LayoutResult RetainedTree::layout(const LayoutInput& root, const LayoutViewport& viewport,
                                  text::TextMeasureCache& cache) {
    RetainedTreeState& state = *state_;
    state.cache = &cache;
    // `apply_style` bakes `vw`/`vh`/`rem` into points and the page adjustments depend on the
    // viewport height, so a viewport change invalidates every applied style. The tree shape and the
    // text are still valid, so only the style pass repeats - not the tree.
    const bool viewport_changed = !state.primed || state.viewport.width != viewport.width ||
                                  state.viewport.height != viewport.height ||
                                  state.viewport.dpi_scale != viewport.dpi_scale ||
                                  state.viewport.font_size != viewport.font_size;
    if (viewport_changed) {
        for (auto& entry : state.nodes) {
            entry.second->style_digest = 0;
        }
    }
    state.viewport = viewport;
    state.primed = true;
    state.restart = false;
    ++state.pass;

    if (state.root != nullptr && state.root->id != root.id) {
        reset(state);
        state.primed = true;
        ++state.pass;
        ++state.rebuilds;
    }

    RetainedNode* retained_root = sync_node(state, root, false);
    if (state.restart || retained_root == nullptr) {
        reset(state);
        ++state.rebuilds;
        return layout_cold(root, viewport, viewport.width, viewport.height, cache,
                           /*as_grid_item=*/false);
    }
    state.root = retained_root;

    // The root is a page (#611): viewport-wide up to `max-width`, centred, at least viewport tall.
    // The condition and the arguments mirror `layout_cold` exactly, so both paths hand Yoga the
    // same root style and the same owner size. Both adjustments are idempotent, and re-applying the
    // root's style above cleared them, so a frame that stops being a page cannot keep them.
    const bool page = viewport.width >= 0.0f && viewport.height >= 0.0f;
    if (page) {
        apply_block_auto_margin_fill(retained_root->node, true);
        apply_root_min_height(retained_root->node, viewport.height);
    }

    YGNodeCalculateLayout(retained_root->node, viewport.width < 0.0f ? YGUndefined : viewport.width,
                          page || viewport.height < 0.0f ? YGUndefined : viewport.height,
                          retained_root->rtl ? YGDirectionRTL : YGDirectionLTR);

    LayoutResult result;
    result.boxes.reserve(state.nodes.size());
    const float origin_x =
        page ? root_auto_margin_offset(retained_root->node, viewport.width) : 0.0f;
    collect_retained_boxes(*retained_root, origin_x, 0.0f, state, result);
    return result;
}

std::size_t RetainedTree::node_count() const {
    return state_ != nullptr ? state_->nodes.size() : 0;
}

std::size_t RetainedTree::style_applications() const {
    return state_ != nullptr ? state_->style_applications : 0;
}

std::size_t RetainedTree::rebuilds() const {
    return state_ != nullptr ? state_->rebuilds : 0;
}

} // namespace tessera::ui::layout::detail
