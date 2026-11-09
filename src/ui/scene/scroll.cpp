// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/scroll.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::ui {
namespace {

float finite(float value) {
    return std::isfinite(value) ? value : 0.0f;
}

float clamp_axis(bool scrollable, float content, float viewport, float value) {
    if (!scrollable) {
        return 0.0f;
    }
    const float limit = std::max(0.0f, content - viewport);
    return std::clamp(finite(value), 0.0f, limit);
}

// Axis-aligned bounds of the four transformed corners (rotate/scale are approximated, matching the
// scene's world_bounds contract).
rhi::Rect transformed_bounds(const rhi::Mat3& transform, const rhi::Rect& rect) {
    const float xs[4] = {rect.x, rect.x + rect.width, rect.x + rect.width, rect.x};
    const float ys[4] = {rect.y, rect.y, rect.y + rect.height, rect.y + rect.height};
    float first_x = 0;
    float first_y = 0;
    transform.transform_point(xs[0], ys[0], first_x, first_y);
    float min_x = first_x;
    float max_x = first_x;
    float min_y = first_y;
    float max_y = first_y;
    for (int i = 1; i < 4; ++i) {
        float x = 0;
        float y = 0;
        transform.transform_point(xs[i], ys[i], x, y);
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_y = std::min(min_y, y);
        max_y = std::max(max_y, y);
    }
    return rhi::Rect{min_x, min_y, max_x - min_x, max_y - min_y};
}

} // namespace

float max_scroll_x(const Node& node) {
    return node.scrollable_x() ? std::max(0.0f, node.content_width() - node.bounds().width) : 0.0f;
}

float max_scroll_y(const Node& node) {
    return node.scrollable_y() ? std::max(0.0f, node.content_height() - node.bounds().height)
                               : 0.0f;
}

bool scroll_to(Node& node, float x, float y) {
    const float next_x =
        clamp_axis(node.scrollable_x(), node.content_width(), node.bounds().width, x);
    const float next_y =
        clamp_axis(node.scrollable_y(), node.content_height(), node.bounds().height, y);
    return node.set_scroll_offset(next_x, next_y);
}

bool scroll_by(Node& node, float dx, float dy) {
    return scroll_to(node, node.scroll_x() + finite(dx), node.scroll_y() + finite(dy));
}

Node* scroll_target(Node& node) {
    for (Node* current = &node; current != nullptr; current = current->parent()) {
        if (current->scrollable_x() || current->scrollable_y()) {
            return current;
        }
    }
    return nullptr;
}

bool apply_wheel_scroll(Node& target, float wheel_x, float wheel_y) {
    Node* const container = scroll_target(target);
    if (container == nullptr) {
        return false;
    }
    return scroll_by(*container, wheel_x * kPixelsPerWheelUnit, wheel_y * kPixelsPerWheelUnit);
}

bool scroll_into_view(Node& node) {
    bool changed = false;
    for (Node* ancestor = node.parent(); ancestor != nullptr; ancestor = ancestor->parent()) {
        if (!ancestor->scrollable_x() && !ancestor->scrollable_y()) {
            continue;
        }
        rhi::Mat3 inverse;
        if (!ancestor->content_transform().try_inverse(inverse)) {
            continue;
        }
        // World rect in the container's content space. The visible content window is
        // [scroll, scroll + viewport] on each axis.
        const rhi::Rect content = transformed_bounds(inverse, node.world_bounds());

        float next_x = ancestor->scroll_x();
        float next_y = ancestor->scroll_y();
        if (ancestor->scrollable_x()) {
            if (content.x < next_x) {
                next_x = content.x;
            } else if (content.x + content.width > next_x + ancestor->bounds().width) {
                next_x = content.x + content.width - ancestor->bounds().width;
            }
        }
        if (ancestor->scrollable_y()) {
            if (content.y < next_y) {
                next_y = content.y;
            } else if (content.y + content.height > next_y + ancestor->bounds().height) {
                next_y = content.y + content.height - ancestor->bounds().height;
            }
        }
        if (scroll_to(*ancestor, next_x, next_y)) {
            changed = true;
        }
    }
    return changed;
}

} // namespace tessera::ui
