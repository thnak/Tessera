// SPDX-License-Identifier: MIT

#include "a11y_text.hpp"
#include "tessera/schema/intrinsics.hpp"
#include "tessera/ui/accessibility/a11y_tree.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace tessera::ui::a11y {
namespace {

using rendering::rhi::Rect;

// Deep trees are bounded rather than recursed without limit; exceeding it reports truncation.
constexpr std::size_t kMaxDepth = 128;

// First aria-* value for `name`, trimmed; empty when the node does not carry it.
std::string_view aria_value(const Node& node, std::string_view name) {
    for (const Node::AriaAttribute& attribute : node.aria_attributes()) {
        if (attribute.name == name) {
            return detail::trim(attribute.value);
        }
    }
    return {};
}

bool aria_hidden(const Node& node) {
    return detail::is_true(aria_value(node, "aria-hidden"));
}

bool aria_modal(const Node& node) {
    return detail::is_true(aria_value(node, "aria-modal"));
}

// The last visible `aria-modal` node in tree order (a nested dialog wins over its ancestor), or
// nullptr. A hidden ancestor hides the dialog with it, so the walk stops at invisible nodes.
const Node* last_modal(const Node& node) {
    if (!node.rendered() || aria_hidden(node)) {
        return nullptr;
    }
    const Node* found = aria_modal(node) ? &node : nullptr;
    for (const auto& child : node.children()) {
        if (const Node* inner = last_modal(*child)) {
            found = inner;
        }
    }
    return found;
}

bool is_ancestor(const Node& ancestor, const Node& node) {
    for (const Node* current = node.parent(); current != nullptr; current = current->parent()) {
        if (current == &ancestor) {
            return true;
        }
    }
    return false;
}

std::string_view paint_text(const Node& node) {
    const auto& data = node.paint_data();
    if (!data || data->kind != PaintKind::Text) {
        return {};
    }
    return data->text;
}

// display:none removes the node and its subtree; visibility:hidden/collapse does the same for this
// slice (the semantics model has no hidden-but-laid-out entry yet).
bool style_hidden(const style::ComputedStyle& style) {
    if (const std::string* display = style.find("display")) {
        if (detail::equal_ci(detail::trim(*display), "none")) {
            return true;
        }
    }
    if (const std::string* visibility = style.find("visibility")) {
        const std::string_view value = detail::trim(*visibility);
        if (detail::equal_ci(value, "hidden") || detail::equal_ci(value, "collapse")) {
            return true;
        }
    }
    return false;
}

bool intersects(const Rect& lhs, const Rect& rhs) {
    const float left = std::max(lhs.x, rhs.x);
    const float top = std::max(lhs.y, rhs.y);
    const float right = std::min(lhs.x + lhs.width, rhs.x + rhs.width);
    const float bottom = std::min(lhs.y + lhs.height, rhs.y + rhs.height);
    return right > left && bottom > top;
}

Rect intersect(const Rect& lhs, const Rect& rhs) {
    const float left = std::max(lhs.x, rhs.x);
    const float top = std::max(lhs.y, rhs.y);
    const float right = std::min(lhs.x + lhs.width, rhs.x + rhs.width);
    const float bottom = std::min(lhs.y + lhs.height, rhs.y + rhs.height);
    return Rect{left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)};
}

// The scroll container's visible world box (border box inset by its scrollport insets).
Rect scrollport_rect(const Node& node) {
    const Rect box = node.world_bounds();
    return Rect{box.x + node.scrollport_left(), box.y + node.scrollport_top(),
                std::max(0.0f, box.width - node.scrollport_left() - node.scrollport_right()),
                std::max(0.0f, box.height - node.scrollport_top() - node.scrollport_bottom())};
}

bool is_scrollable(const Node& node) {
    return node.scrollable_x() || node.scrollable_y();
}

bool focusable_state(const Node& node, bool disabled) {
    return node.focusable() && !disabled;
}

bool disabled_state(const Node& node) {
    if (detail::is_true(aria_value(node, "aria-disabled"))) {
        return true;
    }
    return style::has_state(node.pseudo_state(), style::PseudoState::Disabled);
}

Action actions_for(Role role, bool disabled, bool focusable, bool scrollable) {
    Action actions = Action::None;
    if (focusable) {
        actions = actions | Action::Focus;
    }
    if (!disabled) {
        switch (role) {
        case Role::Button:
        case Role::Checkbox:
        case Role::Radio:
        case Role::Switch:
        case Role::Link:
        case Role::Tab:
        case Role::MenuItem:
            actions = actions | Action::Press;
            break;
        case Role::TextField:
        case Role::Slider:
            actions = actions | Action::SetValue;
            break;
        default:
            break;
        }
        if (scrollable) {
            actions = actions | Action::Scroll;
        }
    }
    return actions;
}

State states_for(Role /*role*/, bool focusable, bool focused, bool disabled, const Node& node) {
    State states = State::None;
    if (focusable) {
        states = states | State::Focusable;
    }
    if (focused) {
        states = states | State::Focused;
    }
    if (disabled) {
        states = states | State::Disabled;
    }
    if (detail::is_true(aria_value(node, "aria-selected"))) {
        states = states | State::Selected;
    }
    const std::string_view checked = aria_value(node, "aria-checked");
    if (style::has_state(node.pseudo_state(), style::PseudoState::Checked) ||
        detail::is_true(checked) || detail::equal_ci(checked, "mixed")) {
        states = states | State::Checked;
    }
    if (detail::is_true(aria_value(node, "aria-expanded"))) {
        states = states | State::Expanded;
    }
    return states;
}

// Explicit role wins (an unknown explicit role degrades to Generic and does not fall back);
// otherwise the schema's impliedRole for the element kind; otherwise Generic.
Role derive_role(const Node& node) {
    if (!node.role().empty()) {
        return role_from_string(node.role());
    }
    if (!node.element_kind().empty()) {
        if (const schema::ElementRow* row = schema::find_element(node.element_kind())) {
            return role_from_string(row->implied_role);
        }
    }
    return Role::Generic;
}

// Copies at most kMaxTextBytes - 1 bytes at a UTF-8 code-point boundary and NUL-terminates.
void store_text(std::array<char, kMaxTextBytes>& out, std::string_view text,
                std::size_t& truncated) {
    if (text.size() >= kMaxTextBytes) {
        std::size_t end = kMaxTextBytes - 1;
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
            --end;
        }
        text = text.substr(0, end);
        ++truncated;
    }
    std::fill(out.begin(), out.end(), '\0');
    std::copy(text.begin(), text.end(), out.begin());
}

class Builder {
public:
    Builder(Node& root, StyleProvider& styles, A11ySnapshot& out, const BuildOptions& options)
        : root_(root),
          styles_(styles),
          out_(out),
          limit_(std::min(options.max_nodes, kMaxNodes)),
          focused_id_(options.focused_id) {}

    BuildStats run() {
        out_.nodes.clear();
        if (out_.nodes.capacity() < limit_) {
            out_.nodes.reserve(limit_);
        }
        modal_ = last_modal(root_);
        std::optional<Rect> clip;
        visit(root_, clip, 0);
        stats_.emitted = out_.nodes.size();
        return stats_;
    }

private:
    void visit(Node& node, const std::optional<Rect>& clip, std::size_t depth) {
        ++stats_.visited;
        if (!node.rendered()) {
            ++stats_.skipped_invisible;
            return;
        }
        if (depth >= kMaxDepth) {
            ++stats_.dropped_nodes;
            stats_.truncated = true;
            return;
        }
        // A modal dialog makes everything that precedes it in tree order inert (its ancestors stay,
        // they lead to it). Content after it paints above it, so toasts and tooltips stay exposed.
        if (&node == modal_) {
            modal_reached_ = true;
        } else if (modal_ != nullptr && !modal_reached_ && !is_ancestor(node, *modal_)) {
            ++stats_.skipped_inert;
            return;
        }
        if (aria_hidden(node)) {
            ++stats_.skipped_aria_hidden;
            return;
        }
        const style::ComputedStyle style = styles_.compute(node);
        if (style_hidden(style)) {
            ++stats_.skipped_style_hidden;
            return;
        }
        const Rect bounds = node.world_bounds();
        if (clip && !intersects(bounds, *clip)) {
            ++stats_.skipped_clipped;
            return;
        }
        if (out_.nodes.size() >= limit_) {
            ++stats_.dropped_nodes;
            stats_.truncated = true;
            return;
        }

        const Role role = derive_role(node);
        if (role == Role::Presentation) {
            // Decorative nodes are ignored together with their subtree (children indices stay
            // exact; there is no hoisting in this slice).
            ++stats_.skipped_decorative;
            return;
        }
        if (bounds.width <= 0.0f || bounds.height <= 0.0f) {
            ++stats_.skipped_zero_size;
            return;
        }

        const std::size_t index = out_.nodes.size();
        out_.nodes.emplace_back();
        fill_node(out_.nodes.back(), node, role, bounds);

        std::optional<Rect> child_clip = clip;
        if (is_scrollable(node)) {
            const Rect scrollport = scrollport_rect(node);
            child_clip = clip ? intersect(*clip, scrollport) : scrollport;
            if (child_clip->width <= 0.0f || child_clip->height <= 0.0f) {
                // Nothing in this scroll container is visible.
                ++stats_.skipped_clipped;
                return;
            }
        }

        for (const auto& child : node.children()) {
            if (stats_.truncated) {
                break;
            }
            A11yNode& entry = out_.nodes[index];
            if (entry.child_count >= kMaxChildren) {
                entry.children_truncated = true;
                ++stats_.truncated_children;
                break;
            }
            const std::size_t before = out_.nodes.size();
            visit(*child, child_clip, depth + 1);
            if (out_.nodes.size() > before) {
                A11yNode& parent = out_.nodes[index];
                parent.children[parent.child_count] = static_cast<A11yIndex>(before);
                ++parent.child_count;
            }
        }
    }

    void fill_node(A11yNode& entry, const Node& node, Role role, const Rect& bounds) {
        const bool disabled = disabled_state(node);
        const bool focusable = focusable_state(node, disabled);
        const bool focused = focused_id_ != 0 && node.id() == focused_id_;
        entry.id = node.id();
        entry.automation_id = node.automation_id();
        entry.role = role;
        entry.states = states_for(role, focusable, focused, disabled, node);
        entry.actions = actions_for(role, disabled, focusable, is_scrollable(node));
        entry.bounds = bounds;
        store_text(entry.name, resolve_name(node, role), stats_.truncated_text);
        store_text(entry.value, resolve_value(node, role), stats_.truncated_text);
        store_text(entry.description, aria_value(node, "aria-description"), stats_.truncated_text);
    }

    // aria-label > aria-labelledby (first resolvable id in this tree) > D9 text > placeholder > id.
    std::string_view resolve_name(const Node& node, Role role) {
        const std::string_view label = aria_value(node, "aria-label");
        if (!label.empty()) {
            return label;
        }
        const std::string_view labelledby = aria_value(node, "aria-labelledby");
        if (!labelledby.empty()) {
            if (const std::string_view resolved = labelledby_name(labelledby); !resolved.empty()) {
                return resolved;
            }
        }
        const std::string_view text = paint_text(node);
        if (!text.empty() && role != Role::TextField && role != Role::Slider) {
            // A text field's text is its value; it is exposed as a last-resort name below instead
            // of shadowing the placeholder/id precedence.
            return text;
        }
        const std::string_view placeholder = aria_value(node, "aria-placeholder");
        if (!placeholder.empty()) {
            return placeholder;
        }
        if (role == Role::TextField || role == Role::Slider) {
            if (!text.empty()) {
                return text;
            }
        }
        return node.element_id();
    }

    std::string_view labelledby_name(std::string_view ids) {
        std::size_t position = 0;
        while (position < ids.size()) {
            const std::size_t first = ids.find_first_not_of(" \t\r\n", position);
            if (first == std::string_view::npos) {
                break;
            }
            const std::size_t last = ids.find_first_of(" \t\r\n", first);
            const std::string_view token =
                ids.substr(first, (last == std::string_view::npos ? ids.size() : last) - first);
            if (Node* referenced = find_by_element_id(root_, token)) {
                const std::string_view label = aria_value(*referenced, "aria-label");
                if (!label.empty()) {
                    return label;
                }
                const std::string_view text = paint_text(*referenced);
                if (!text.empty()) {
                    return text;
                }
            }
            if (last == std::string_view::npos) {
                break;
            }
            position = last;
        }
        return {};
    }

    static std::string_view resolve_value(const Node& node, Role role) {
        if (role == Role::TextField) {
            return paint_text(node);
        }
        if (role == Role::Slider || role == Role::Progress) {
            const std::string_view text = aria_value(node, "aria-valuetext");
            if (!text.empty()) {
                return text;
            }
            return aria_value(node, "aria-valuenow");
        }
        return {};
    }

    static Node* find_by_element_id(Node& node, std::string_view id) {
        if (node.element_id() == id) {
            return &node;
        }
        for (const auto& child : node.children()) {
            if (Node* const found = find_by_element_id(*child, id)) {
                return found;
            }
        }
        return nullptr;
    }

    Node& root_;
    StyleProvider& styles_;
    A11ySnapshot& out_;
    std::size_t limit_;
    NodeId focused_id_;
    const Node* modal_ = nullptr;
    bool modal_reached_ = false;
    BuildStats stats_;
};

} // namespace

std::string_view name_of(const A11yNode& node) {
    return std::string_view(node.name.data());
}

std::string_view value_of(const A11yNode& node) {
    return std::string_view(node.value.data());
}

std::string_view description_of(const A11yNode& node) {
    return std::string_view(node.description.data());
}

BuildStats build_a11y_tree(Node& root, StyleProvider& styles, A11ySnapshot& out,
                           const BuildOptions& options) {
    Builder builder(root, styles, out, options);
    return builder.run();
}

A11yIndex find_by_automation_id(const A11ySnapshot& snapshot, std::uint64_t automation_id) {
    if (automation_id == 0) {
        return kInvalidIndex;
    }
    for (std::size_t index = 0; index < snapshot.nodes.size(); ++index) {
        if (snapshot.nodes[index].automation_id == automation_id) {
            return static_cast<A11yIndex>(index);
        }
    }
    return kInvalidIndex;
}

A11yIndex find_by_node_id(const A11ySnapshot& snapshot, NodeId id) {
    if (id == 0) {
        return kInvalidIndex;
    }
    for (std::size_t index = 0; index < snapshot.nodes.size(); ++index) {
        if (snapshot.nodes[index].id == id) {
            return static_cast<A11yIndex>(index);
        }
    }
    return kInvalidIndex;
}

} // namespace tessera::ui::a11y
