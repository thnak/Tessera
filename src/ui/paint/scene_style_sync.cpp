// SPDX-License-Identifier: MIT
//
// Computed style -> scene graph, for the properties the graph (not the paint pass) consumes:
// z-index, transform, box-shadow reach and the glass backdrop sigma. A steady scene stays clean:
// every setter is a no-op when the value it stores is unchanged.

#include "paint_properties.hpp"
#include "paint_shadow.hpp"
#include "paint_text_shadow.hpp"
#include "tessera/ui/paint/scene_style_sync.hpp"
#include "tessera/ui/style/transform.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <string>
#include <string_view>

namespace tessera::ui::paint {
namespace {

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n\f");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n\f");
    return text.substr(first, last - first + 1);
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

// Returns whether this node's subtree contains a glass node (itself included).
bool sync_node(Node& node, StyleProvider& styles, const rendering::gpu::GlassLimits& glass,
               style::SyncMemo* memo, SceneStyleSyncResult& result) {
    if (!node.rendered()) {
        return false;
    }
    const style::ComputedStyle& style = styles.resolve(node);
    ++result.nodes;
    if (memo != nullptr && memo->hit(node.id(), style, node.bounds().width, node.bounds().height)) {
        // Applied already with this exact style and box: only the subtree walk is left.
        const float applied = node.glass_blur_sigma();
        if (applied > 0.0f) {
            ++result.glass_nodes;
        }
        bool subtree_has_glass = applied > 0.0f;
        for (const auto& child : node.children()) {
            subtree_has_glass = sync_node(*child, styles, glass, memo, result) || subtree_has_glass;
        }
        node.set_glass_subtree(subtree_has_glass);
        return subtree_has_glass;
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
    const Node::PaintOutsets outsets{
        std::max(reach.left, text_reach.left), std::max(reach.top, text_reach.top),
        std::max(reach.right, text_reach.right), std::max(reach.bottom, text_reach.bottom),
        reach.blurred || text_reach.blurred};
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
    if (sigma != node.glass_blur_sigma()) {
        node.set_glass_blur_sigma(sigma);
        ++result.changed_glass;
    }
    if (sigma > 0.0f) {
        ++result.glass_nodes;
    }

    if (memo != nullptr) {
        memo->put(node.id(), style, node.bounds().width, node.bounds().height);
    }
    bool subtree_has_glass = sigma > 0.0f;
    for (const auto& child : node.children()) {
        subtree_has_glass = sync_node(*child, styles, glass, memo, result) || subtree_has_glass;
    }
    node.set_glass_subtree(subtree_has_glass);
    return subtree_has_glass;
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

} // namespace tessera::ui::paint
