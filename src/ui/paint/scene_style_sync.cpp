// SPDX-License-Identifier: MIT
//
// Computed style -> scene graph, for the properties the graph (not the paint pass) consumes:
// z-index, transform, box-shadow reach and the glass backdrop sigma. A steady scene stays clean:
// every setter is a no-op when the value it stores is unchanged.

#include "paint_filter.hpp"
#include "paint_properties.hpp"
#include "paint_shadow.hpp"
#include "paint_text_shadow.hpp"
#include "tessera/detail/text_util.hpp"
#include "tessera/ui/paint/scene_style_sync.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/transform.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::paint {

using tessera::detail::trim;
namespace {

// How far `node` is from the root, for ordering the scoped sync's ancestor refold.
std::size_t depth_of(const Node& node) {
    std::size_t depth = 0;
    for (const Node* parent = node.parent(); parent != nullptr; parent = parent->parent()) {
        ++depth;
    }
    return depth;
}

// `auto` and anything that is not an integer resolve to 0, the CSS stacking default.
int z_index_value(const style::ComputedStyle& style) {
    const std::string* value = style.find("z-index");
    if (value == nullptr) {
        return 0;
    }
    const std::string token(trim(*value));
    if (token.empty()) {
        return 0;
    }
    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(token.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || errno == ERANGE) {
        return 0;
    }
    return static_cast<int>(std::clamp<long>(parsed, INT_MIN, INT_MAX));
}

// Which dependency kinds a subtree contains (the node itself included).
struct SubtreeLayers {
    bool glass = false;
    bool filter = false;

    void merge(const SubtreeLayers& other) {
        glass = glass || other.glass;
        filter = filter || other.filter;
    }
};

// Folds a node's own facts and its children's flags into the node's two subtree bits.
SubtreeLayers fold_subtree(Node& node, SubtreeLayers layers) {
    for (const auto& child : node.children()) {
        layers.merge({child->glass_subtree(), child->filter_subtree()});
    }
    node.set_glass_subtree(layers.glass);
    node.set_filter_subtree(layers.filter);
    return layers;
}

SubtreeLayers sync_node(Node& node, StyleProvider& styles, const rendering::gpu::GlassLimits& glass,
                        style::SyncMemo* memo, SceneStyleSyncResult& result) {
    if (!node.rendered()) {
        return {};
    }
    const style::ComputedStyle& style = styles.resolve(node);
    ++result.nodes;
    if (memo != nullptr && memo->hit(node.id(), style, node.bounds().width, node.bounds().height)) {
        // Applied already with this exact style and box: only the subtree walk is left.
        SubtreeLayers layers{node.glass_blur_sigma() > 0.0f, node.filter_reach() > 0.0f};
        result.glass_nodes += layers.glass ? 1 : 0;
        result.filter_nodes += layers.filter ? 1 : 0;
        for (const auto& child : node.children()) {
            layers.merge(sync_node(*child, styles, glass, memo, result));
        }
        node.set_glass_subtree(layers.glass);
        node.set_filter_subtree(layers.filter);
        return layers;
    }

    const int z_index = z_index_value(style);
    if (z_index != node.z_index()) {
        node.set_z_index(z_index);
        ++result.changed_z_index;
    }

    const std::string* transform = style.find("transform");
    const std::string* origin = style.find("transform-origin");
    const rendering::rhi::Rect& bounds = node.bounds();
    const std::optional<rendering::rhi::Mat3> matrix = style::compute_transform(
        transform != nullptr ? *transform : "none", origin != nullptr ? *origin : "50% 50%",
        bounds.width, bounds.height);
    const rendering::rhi::Mat3 next = matrix.value_or(rendering::rhi::Mat3::identity());
    if (!std::equal(next.m, next.m + 9, node.transform().m)) {
        node.set_transform(next);
        ++result.changed_transform;
    }

    const detail::ShadowOutsets reach = detail::shadow_outsets(style);
    // `text-shadow` (issue #616) reaches beyond the text box too, and it is a property of the style
    // alone (not of the glyphs), so it folds into the same paint outsets. That is what keeps the
    // culling reach and the damage region correct without ever growing the glyph ink box the fit
    // and culling code reads.
    const detail::TextShadowOutsets text_reach = detail::text_shadow_outsets(style);
    // `filter` (issue #617) reaches past the box too: its blur halo and drop-shadow offset. The
    // layer holds the node's own shadows, so the chain blurs them as well: the halo is added to the
    // content's reach on each side, not maxed with it.
    const detail::ShadowOutsets filter_halo = detail::filter_outsets(style, glass);
    const Node::PaintOutsets outsets{std::max(reach.left, text_reach.left) + filter_halo.left,
                                     std::max(reach.top, text_reach.top) + filter_halo.top,
                                     std::max(reach.right, text_reach.right) + filter_halo.right,
                                     std::max(reach.bottom, text_reach.bottom) + filter_halo.bottom,
                                     reach.blurred || text_reach.blurred || filter_halo.blurred};
    const Node::PaintOutsets& current = node.paint_outsets();
    if (outsets.left != current.left || outsets.top != current.top ||
        outsets.right != current.right || outsets.bottom != current.bottom ||
        outsets.blurred != current.blurred) {
        node.set_paint_outsets(outsets);
        ++result.changed_outsets;
    }

    // Glass: the resolved sigma the damage dependency region is derived from. The clamp uses the
    // configured limit, so the stored value is exactly what paint will emit.
    const std::optional<style::BackdropFilter> filter = detail::backdrop_filter_value(style);
    const float sigma =
        filter.has_value() ? rendering::gpu::clamp_glass_sigma(filter->blur_sigma, glass) : 0.0f;
    // The stored values are fixed point, so "changed" is read back from the node, not guessed by
    // comparing the unrounded input with the rounded store.
    const float stored_sigma = node.glass_blur_sigma();
    node.set_glass_blur_sigma(sigma);
    if (node.glass_blur_sigma() != stored_sigma) {
        ++result.changed_glass;
    }

    // Layer filter: its own dependency, never the backdrop sigma. The largest one-sided halo is the
    // conservative symmetric reach (a drop-shadow's offset makes the halo one-sided).
    const float filter_reach =
        std::max({filter_halo.left, filter_halo.top, filter_halo.right, filter_halo.bottom});
    const float stored_reach = node.filter_reach();
    node.set_filter_reach(filter_reach);
    if (node.filter_reach() != stored_reach) {
        ++result.changed_filter;
    }
    SubtreeLayers layers{node.glass_blur_sigma() > 0.0f, node.filter_reach() > 0.0f};
    result.glass_nodes += layers.glass ? 1 : 0;
    result.filter_nodes += layers.filter ? 1 : 0;

    if (memo != nullptr) {
        memo->put(node.id(), style, node.bounds().width, node.bounds().height);
    }
    for (const auto& child : node.children()) {
        layers.merge(sync_node(*child, styles, glass, memo, result));
    }
    node.set_glass_subtree(layers.glass);
    node.set_filter_subtree(layers.filter);
    return layers;
}

} // namespace

SceneStyleSyncResult sync_scene_style(Node& root, StyleProvider& styles,
                                      const rendering::gpu::GlassLimits& glass,
                                      style::SyncMemo* memo) {
    SceneStyleSyncResult result;
    if (memo != nullptr) {
        memo->begin();
    }
    sync_node(root, styles, glass, memo, result);
    if (memo != nullptr) {
        memo->sweep();
    }
    return result;
}

SceneStyleSyncResult sync_scene_style_scoped(StyleProvider& styles,
                                             std::span<const Node* const> roots,
                                             const rendering::gpu::GlassLimits& glass,
                                             style::SyncMemo* memo) {
    SceneStyleSyncResult result;
    if (roots.empty()) {
        return result;
    }
    if (memo != nullptr) {
        memo->begin();
    }
    // The ancestors of every root, nearest-last, so `glass_subtree` and `filter_subtree` refold
    // bottom-up. A root's ancestors did not restyle, but those facts depend on their descendants,
    // so they must be revisited after the roots below them.
    std::vector<Node*> ancestors;
    for (const Node* root_node : roots) {
        for (Node* parent = root_node->parent(); parent != nullptr; parent = parent->parent()) {
            if (std::find(ancestors.begin(), ancestors.end(), parent) == ancestors.end()) {
                ancestors.push_back(parent);
            }
        }
    }
    // Deepest first, so a parent's refold sees its children's final value.
    std::sort(ancestors.begin(), ancestors.end(),
              [](const Node* a, const Node* b) { return depth_of(*a) > depth_of(*b); });
    for (const Node* root_node : roots) {
        sync_node(const_cast<Node&>(*root_node), styles, glass, memo, result);
    }
    for (Node* ancestor : ancestors) {
        // Only the subtree refold is owed here: the ancestor's own style and box are unchanged, so
        // its z-index/transform/outsets are already applied. Re-running the full sync would be
        // correct but would re-derive what the memo already proves.
        fold_subtree(*ancestor,
                     {ancestor->glass_blur_sigma() > 0.0f, ancestor->filter_reach() > 0.0f});
    }
    if (memo != nullptr) {
        memo->sweep();
    }
    return result;
}

} // namespace tessera::ui::paint
