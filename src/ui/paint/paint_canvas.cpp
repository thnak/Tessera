// SPDX-License-Identifier: MIT
//
// The canvas colour of a scene (split out of paint_pass.cpp, which holds the node walk).

#include "paint_properties.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"

#include <optional>

namespace tessera::ui::paint {

// The canvas background. A browser paints the root element's background over the whole canvas, and
// when the root has none, the first child's (`body`) takes its place, so a page colour reaches
// every edge of the window however short the content is. The window root here is the canvas: it is
// the size of the window, its child is the app's root view, and the app's own children are the
// author's code. It is painted under the root's own box, before everything, so the app's root view
// keeps the size its layout gave it and only its colour is extended. An opaque colour only: a
// translucent one would be blended twice (canvas, then the child's own box).
std::optional<rhi::Color> canvas_background(const Node& root, StyleProvider& styles) {
    if (!root.rendered() || root.children().empty()) {
        return std::nullopt;
    }
    const auto background = [&styles](const Node& node) {
        const style::ComputedStyle& style = styles.resolve(node);
        return detail::color_value(style, "background-color", detail::current_color(style));
    };
    // The root's own background wins, translucent or not: its own pass paints it.
    if (const auto own = background(root); own && own->a > 0.0f) {
        return std::nullopt;
    }
    const Node& first = *root.children().front();
    if (!first.rendered()) {
        return std::nullopt;
    }
    const std::optional<rhi::Color> color = background(first);
    if (!color || color->a < 1.0f) {
        return std::nullopt;
    }
    return color;
}

} // namespace tessera::ui::paint
