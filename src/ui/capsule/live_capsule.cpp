// SPDX-License-Identifier: MIT

#include "tessera/ui/capsule/live_capsule.hpp"
#include "tessera/ui/scene/event.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string_view>

namespace tessera::ui::capsule {

namespace {

// Clear of the overlay host's own range (1 << 62) and of generated ids.
constexpr NodeId kFirstPartId = NodeId{1} << 61;
NodeId next_part_id = kFirstPartId;

Node& add_part(Node& parent, std::string_view part, std::string_view label = {}) {
    Node& node = parent.add_child(std::make_unique<Node>(next_part_id++));
    node.set_element_kind("View");
    node.set_class_name(part);
    if (!label.empty()) {
        node.set_aria("aria-label", label);
    }
    return node;
}

// The visible text of a part (the title, an action label); the accessible name stays on the part.
void add_text(Node& parent, std::string_view text) {
    if (text.empty()) {
        return;
    }
    Node& node = parent.add_child(std::make_unique<Node>(next_part_id++));
    node.set_element_kind("Text");
    PaintData data;
    data.kind = PaintKind::Text;
    data.text = std::string(text);
    node.set_paint_data(std::move(data));
}

bool same_rect(const hal::WindowRect& a, const hal::WindowRect& b) noexcept {
    return std::fabs(a.x - b.x) < 0.01f && std::fabs(a.y - b.y) < 0.01f &&
           std::fabs(a.width - b.width) < 0.01f && std::fabs(a.height - b.height) < 0.01f;
}

} // namespace

LiveCapsule::LiveCapsule(LiveCapsuleHost& host, OverlayHost& overlays)
    : host_(host),
      overlays_(overlays) {}

LiveCapsule::~LiveCapsule() {
    // `abandon` leaves no handle, so this closes nothing and never touches the host.
    close();
}

void LiveCapsule::set_px(Node& node, std::size_t slot, std::string_view property, float value) {
    PxText& storage = px_[slot];
    std::array<char, 24>& buffer = storage.text[storage.next];
    storage.next ^= 1U;
    std::snprintf(buffer.data(), buffer.size(), "%ldpx", std::lround(value));
    node.set_inline_style(property, std::string_view(buffer.data()));
}

void LiveCapsule::close() {
    if (handle_) {
        overlays_.close(handle_);
        handle_ = {};
    }
    action_fns_.clear();
    parts_dirty_ = true;
    box_dirty_ = true;
    built_revision_ = ~std::uint64_t{0};
}

void LiveCapsule::rebuild(Node& content, const Activity* front, const Activity* minimal) {
    while (!content.children().empty()) {
        static_cast<void>(content.remove_child(*content.children().back()));
    }
    action_fns_.clear();

    const Activity* named = front != nullptr ? front : minimal;
    content.set_aria("aria-label", named != nullptr ? named->label : std::string_view{});
    content.set_aria("aria-expanded",
                     controller_.state() == CapsuleState::Expanded ? "true" : "false");

    if (front != nullptr && controller_.state() == CapsuleState::Expanded) {
        Node& card = add_part(content, "card");
        add_text(add_part(card, "title", front->slots.title), front->slots.title);
        Node& body = add_part(card, "content");
        if (front->slots.content) {
            front->slots.content(body);
        }
        Node& actions = add_part(card, "actions");
        for (const ActionSlot& action : front->slots.actions) {
            Node& button = add_part(actions, "action", action.label);
            add_text(button, action.label);
            button.set_role("button");
            button.set_focusable(true);
            button.set_tab_index(0);
            if (action.on_press) {
                action_fns_.push_back(action.on_press);
                button.bind_event(
                    EventType::Click,
                    [](void* context, const Event&) {
                        (*static_cast<std::function<void()>*>(context))();
                    },
                    &action_fns_.back());
            }
        }
    } else if (front != nullptr) {
        Node& leading = add_part(content, "leading");
        if (front->slots.leading) {
            front->slots.leading(leading);
        }
        Node& trailing = add_part(content, "trailing");
        if (front->slots.trailing) {
            front->slots.trailing(trailing);
        }
    }
    if (minimal != nullptr && controller_.state() != CapsuleState::Expanded) {
        Node& icon = add_part(content, "minimal-icon", minimal->label);
        if (minimal->slots.minimal_icon) {
            minimal->slots.minimal_icon(icon);
        }
    }
}

bool LiveCapsule::sync(const LiveCapsuleEnv& env, float dt) {
    const Activity* front = host_.front();
    const Activity* minimal = host_.minimal();
    if (front == nullptr && minimal == nullptr) {
        if (handle_) {
            close();
            generation_ = ~std::uint64_t{0};
        }
        return false;
    }

    if (!handle_) {
        OverlaySpec spec;
        spec.id = "live-capsule";
        spec.layer = OverlayLayer::Toast;
        spec.dismiss_on_escape = false;
        spec.dismiss_on_outside = false;
        spec.persistent = true;
        spec.mount = [this](Node& root) {
            // The toast layer says "alert"; the capsule is a status. It is focusable and in the tab
            // order, but nothing ever focuses it on its own.
            root.set_role("status");
            root.set_class_name("capsule");
            root.set_focusable(true);
            root.set_tab_index(0);
            root.set_inline_style("position", "absolute");
            root.bind_event(
                EventType::Click,
                [](void* context, const Event&) {
                    static_cast<LiveCapsule*>(context)->activate(0);
                },
                this);
        };
        handle_ = overlays_.open(std::move(spec));
    }
    Node* content = overlays_.content(handle_);
    if (content == nullptr) {
        return true; // mounts on the next flush
    }

    gesture_inset_top_ = env.gesture_inset_top;
    morph_.set_reduced_motion(env.reduced_motion);

    if (host_.front_generation() != generation_) {
        generation_ = host_.front_generation();
        controller_.reset(front != nullptr ? CapsuleState::Compact : CapsuleState::Minimal);
        parts_dirty_ = true;
    }
    if (front == nullptr && controller_.state() != CapsuleState::Minimal) {
        controller_.reset(CapsuleState::Minimal);
        parts_dirty_ = true;
    }

    const Activity* sized = front != nullptr ? front : minimal;
    hal::WindowRect cutout;
    const bool has_cutout = top_cutout(env.cutouts, env.window_width, cutout);
    CapsuleSizeInput size_in;
    size_in.state = controller_.state();
    size_in.window_width = env.window_width;
    size_in.window_height = env.window_height;
    size_in.cutout_width = has_cutout ? cutout.width : 0.0f;
    size_in.cutout_height = has_cutout ? cutout.height : 0.0f;
    size_in.leading_width = sized->slots.leading_width;
    size_in.trailing_width = sized->slots.trailing_width;
    size_in.card_width = sized->slots.card_width;
    size_in.card_height = sized->slots.card_height;
    const CapsuleSize target = capsule_size(size_in);
    if (!morph_placed_) {
        morph_.snap(target);
        morph_placed_ = true;
        box_dirty_ = true;
    } else if (target.width != target_.width || target.height != target_.height ||
               target.corner_radius != target_.corner_radius) {
        morph_.set_target(target);
    }
    target_ = target;
    if (built_state_ != controller_.state()) {
        built_state_ = controller_.state();
        parts_dirty_ = true;
    }

    if (parts_dirty_ || built_revision_ != host_.revision()) {
        rebuild(*content, front, minimal);
        built_revision_ = host_.revision();
        parts_dirty_ = false;
        box_dirty_ = true;
    }

    const bool moving = morph_.tick(dt);
    const CapsuleSize now = morph_.current();
    CapsulePlacementInput place;
    place.window_width = env.window_width;
    place.window_height = env.window_height;
    place.top_inset = env.top_inset;
    place.cutouts = env.cutouts;
    place.hinge = env.hinge;
    place.width = now.width;
    place.height = now.height;
    const hal::WindowRect rect = place_capsule(place).rect;
    if (box_dirty_ || moving || !same_rect(rect, rect_)) {
        rect_ = rect;
        set_px(*content, 0, "left", rect.x);
        set_px(*content, 1, "top", rect.y);
        set_px(*content, 2, "width", rect.width);
        set_px(*content, 3, "height", rect.height);
        set_px(*content, 4, "border-radius", now.corner_radius);
        box_dirty_ = false;
    }
    return moving || morph_.active();
}

bool LiveCapsule::activate(std::uint64_t focused_node) {
    const bool changed = controller_.activate(focused_node);
    parts_dirty_ = parts_dirty_ || changed;
    return changed;
}

bool LiveCapsule::escape() {
    const bool changed = controller_.escape();
    parts_dirty_ = parts_dirty_ || changed;
    return changed;
}

bool LiveCapsule::pointer_down(float x, float y) {
    const bool inside =
        x >= rect_.x && x < rect_.x + rect_.width && y >= rect_.y && y < rect_.y + rect_.height;
    if (inside) {
        return false;
    }
    const bool changed = controller_.outside_press();
    parts_dirty_ = parts_dirty_ || changed;
    return changed;
}

bool LiveCapsule::swipe_up(float start_y) {
    const bool changed = controller_.swipe_up(start_y, gesture_inset_top_);
    parts_dirty_ = parts_dirty_ || changed;
    return changed;
}

} // namespace tessera::ui::capsule
