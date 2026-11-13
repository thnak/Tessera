// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/scroll.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace tessera::ui {
namespace {

float finite(float value) {
    return std::isfinite(value) ? value : 0.0f;
}

// Re-entrancy guard for Scroll emission. The dispatcher itself has no dispatch-in-progress state,
// so a handler that calls `scroll_by` on the same container would otherwise synthesize another
// Scroll event for every nested move and recurse without bound. The nested move still applies; only
// its event is suppressed. Entries are per node, so a handler may scroll a different container and
// get that container's event. Bounded and allocation-free like the dispatcher; UI-thread only.
constexpr std::size_t kMaxScrollEventDepth = 8;
Node* g_scroll_event_stack[kMaxScrollEventDepth] = {};
std::size_t g_scroll_event_depth = 0;

bool scroll_event_active(const Node& node) {
    for (std::size_t i = 0; i < g_scroll_event_depth; ++i) {
        if (g_scroll_event_stack[i] == &node) {
            return true;
        }
    }
    return false;
}

// Pops the guard on scope exit, including an exception thrown by a user handler (error boundaries
// catch those, but the guard must not stay armed).
class ScrollEventScope {
public:
    explicit ScrollEventScope(Node& node)
        : pushed_(g_scroll_event_depth < kMaxScrollEventDepth) {
        if (pushed_) {
            g_scroll_event_stack[g_scroll_event_depth++] = &node;
        }
    }
    ~ScrollEventScope() {
        if (pushed_) {
            --g_scroll_event_depth;
        }
    }
    ScrollEventScope(const ScrollEventScope&) = delete;
    ScrollEventScope& operator=(const ScrollEventScope&) = delete;

    [[nodiscard]] bool pushed() const {
        return pushed_;
    }

private:
    bool pushed_;
};

// Emits the scene Scroll event for one applied movement. The container is the dispatch target, so
// its ancestors see the capture and bubble phases; the payload is filled from the node's real
// scrollport/content metrics.
void emit_scroll_event(Node& node, float delta_x, float delta_y) {
    if (scroll_event_active(node)) {
        return;
    }
    ScrollEventScope scope(node);
    if (!scope.pushed()) {
        // Nested past the bound: the movement is already applied and the event is dropped rather
        // than growing the guard.
        return;
    }
    Event event;
    event.type = EventType::Scroll;
    event.scroll_x = delta_x;
    event.scroll_y = delta_y;
    event.scroll_offset_x = node.scroll_x();
    event.scroll_offset_y = node.scroll_y();
    event.scroll_viewport_width = scrollport_width(node);
    event.scroll_viewport_height = scrollport_height(node);
    event.scroll_content_width = node.content_width();
    event.scroll_content_height = node.content_height();
    dispatch_event_to(node, event);
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

float scrollport_width(const Node& node) {
    return std::max(0.0f, node.bounds().width - node.scrollport_left() - node.scrollport_right());
}

float scrollport_height(const Node& node) {
    return std::max(0.0f, node.bounds().height - node.scrollport_top() - node.scrollport_bottom());
}

float max_scroll_x(const Node& node) {
    return node.scrollable_x() ? std::max(0.0f, node.content_width() - scrollport_width(node))
                               : 0.0f;
}

float max_scroll_y(const Node& node) {
    return node.scrollable_y() ? std::max(0.0f, node.content_height() - scrollport_height(node))
                               : 0.0f;
}

bool scroll_to(Node& node, float x, float y) {
    const float before_x = node.scroll_x();
    const float before_y = node.scroll_y();
    const float next_x =
        clamp_axis(node.scrollable_x(), node.content_width(), scrollport_width(node), x);
    const float next_y =
        clamp_axis(node.scrollable_y(), node.content_height(), scrollport_height(node), y);
    if (!node.set_scroll_offset(next_x, next_y)) {
        return false;
    }
    // Exactly one event per applied movement; the delta is what the container actually applied
    // after clamping, so a request that only clamps emits nothing (set_scroll_offset returned
    // false above).
    emit_scroll_event(node, node.scroll_x() - before_x, node.scroll_y() - before_y);
    return true;
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

namespace {

Node* find_node_by_id(Node& node, NodeId id) {
    if (node.id() == id) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (Node* const found = find_node_by_id(*child, id)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

Node* scroll_target_for_id(Node& root, NodeId id) {
    if (id == 0) {
        return nullptr;
    }
    Node* const target = find_node_by_id(root, id);
    return target == nullptr ? nullptr : scroll_target(*target);
}

bool allow_chain(const Node& node) {
    // CSS `overscroll-behavior` (scroll-and-gestures-v1 section 2.1): `contain`/`none` stop the
    // scroll chain at this container; there is no separate `scroll-chain` property.
    return node.overscroll_behavior() == OverscrollBehavior::Auto;
}

bool animates_overscroll(const Node& node) {
    if (node.overscroll_effect() == OverscrollEffect::None || !allow_chain(node)) {
        return false;
    }
    // `Platform` is a request for the HAL profile; the profile resolves None/Glow/Stretch/Bounce at
    // the consumer. A non-`none` request is eligible to animate here.
    return true;
}

namespace {

// Applies one wheel delta from the innermost container outward (scroll-and-gestures-v1 section 4):
// each container consumes what it can and passes the remainder to its nearest scrollable ancestor
// while `allow_chain` holds. `contain`/`none` drop the remainder at that edge. Bounded by tree
// depth, allocation-free, one Scroll event per container that actually moved.
bool chain_scroll_delta(Node& innermost, float dx, float dy) {
    bool changed = false;
    Node* container = &innermost;
    while (container != nullptr) {
        const float before_x = container->scroll_x();
        const float before_y = container->scroll_y();
        (void)scroll_by(*container, dx, dy);
        const float consumed_x = container->scroll_x() - before_x;
        const float consumed_y = container->scroll_y() - before_y;
        changed = changed || consumed_x != 0.0f || consumed_y != 0.0f;
        dx -= consumed_x;
        dy -= consumed_y;
        if (!allow_chain(*container) || (dx == 0.0f && dy == 0.0f)) {
            break;
        }
        Node* const parent = container->parent();
        container = parent == nullptr ? nullptr : scroll_target(*parent);
    }
    return changed;
}

} // namespace

bool apply_wheel_scroll(Node& target, float wheel_x, float wheel_y) {
    Node* container = scroll_target(target);
    if (container == nullptr) {
        return false;
    }
    float dx = wheel_x * kPixelsPerWheelUnit;
    float dy = wheel_y * kPixelsPerWheelUnit;
    return chain_scroll_delta(*container, dx, dy);
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
        // World rect in the container's content space. A content point is visible when it is inside
        // the scrollport window [scroll + inset_start, scroll + extent - inset_end], where the
        // extent is the border box inset by both scrollport insets on that axis.
        const rhi::Rect content = transformed_bounds(inverse, node.world_bounds());
        const float left = ancestor->scrollport_left();
        const float top = ancestor->scrollport_top();
        const float extent_x = scrollport_width(*ancestor);
        const float extent_y = scrollport_height(*ancestor);

        float next_x = ancestor->scroll_x();
        float next_y = ancestor->scroll_y();
        if (ancestor->scrollable_x() && extent_x > 0.0f) {
            if (content.x < next_x + left) {
                // Starts before the window: align its start edge (an oversized node keeps the top
                // or left edge visible, matching the pre-inset behaviour).
                next_x = content.x - left;
            } else if (content.x + content.width > next_x + left + extent_x) {
                next_x = content.x + content.width - left - extent_x;
            }
        }
        if (ancestor->scrollable_y() && extent_y > 0.0f) {
            if (content.y < next_y + top) {
                next_y = content.y - top;
            } else if (content.y + content.height > next_y + top + extent_y) {
                next_y = content.y + content.height - top - extent_y;
            }
        }
        if (scroll_to(*ancestor, next_x, next_y)) {
            changed = true;
        }
    }
    return changed;
}

} // namespace tessera::ui
