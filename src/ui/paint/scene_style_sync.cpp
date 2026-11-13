// SPDX-License-Identifier: MIT

#include "paint_shadow.hpp"
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

void sync_node(Node& node, StyleProvider& styles, SceneStyleSyncResult& result) {
    if (!node.visible()) {
        return;
    }
    const style::ComputedStyle style = styles.compute(node);
    ++result.nodes;

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
    const Node::PaintOutsets outsets{reach.left, reach.top, reach.right, reach.bottom,
                                     reach.blurred};
    const Node::PaintOutsets& current = node.paint_outsets();
    if (outsets.left != current.left || outsets.top != current.top ||
        outsets.right != current.right || outsets.bottom != current.bottom ||
        outsets.blurred != current.blurred) {
        node.set_paint_outsets(outsets);
        ++result.changed_outsets;
    }

    for (const auto& child : node.children()) {
        sync_node(*child, styles, result);
    }
}

} // namespace

SceneStyleSyncResult sync_scene_style(Node& root, StyleProvider& styles) {
    SceneStyleSyncResult result;
    sync_node(root, styles, result);
    return result;
}

} // namespace tessera::ui::paint
