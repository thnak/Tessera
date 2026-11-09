// SPDX-License-Identifier: MIT
//
// Shared fixtures for the accessibility semantics tests. Included by the two test translation
// units; the builder source itself lives under src/ui/accessibility.

#pragma once

#include "tessera/ui/accessibility/a11y_tree.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/scroll.hpp"

#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace a11y_test {

using tessera::rendering::rhi::Rect;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::a11y::A11yIndex;
using tessera::ui::a11y::A11yNode;
using tessera::ui::a11y::A11ySnapshot;
using tessera::ui::a11y::Action;
using tessera::ui::a11y::BuildOptions;
using tessera::ui::a11y::BuildStats;
using tessera::ui::a11y::kInvalidIndex;
using tessera::ui::a11y::kMaxChildren;
using tessera::ui::a11y::name_of;
using tessera::ui::a11y::Role;
using tessera::ui::a11y::role_from_string;
using tessera::ui::a11y::State;
using tessera::ui::a11y::StyleProvider;
using tessera::ui::a11y::value_of;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::PseudoState;

inline bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "a11y_tree: " << message << "\n";
    }
    return condition;
}

inline ComputedStyle styled(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle style;
    for (const auto& [property, value] : values) {
        style.set(property, value);
    }
    return style;
}

class StubStyles final : public StyleProvider {
public:
    void set(NodeId id, ComputedStyle style) {
        styles_[id] = std::move(style);
    }

    [[nodiscard]] ComputedStyle compute(const Node& node) override {
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? ComputedStyle{} : found->second;
    }

private:
    std::map<NodeId, ComputedStyle> styles_;
};

inline Node& child(Node& parent, NodeId id) {
    auto owner = std::make_unique<Node>(id);
    // A non-zero default size keeps the zero-size skip rule out of tests that only care about
    // semantics; tests override it explicitly when they exercise that rule.
    owner->set_bounds({0.0f, 0.0f, 10.0f, 10.0f});
    return parent.add_child(std::move(owner));
}

inline tessera::ui::PaintData text_payload(const char* text) {
    tessera::ui::PaintData data;
    data.kind = tessera::ui::PaintKind::Text;
    data.text = text;
    return data;
}

inline bool same_node(const A11yNode& lhs, const A11yNode& rhs) {
    return lhs.id == rhs.id && lhs.automation_id == rhs.automation_id && lhs.role == rhs.role &&
           lhs.states == rhs.states && lhs.actions == rhs.actions && name_of(lhs) == name_of(rhs) &&
           value_of(lhs) == value_of(rhs) && lhs.bounds.x == rhs.bounds.x &&
           lhs.bounds.y == rhs.bounds.y && lhs.bounds.width == rhs.bounds.width &&
           lhs.bounds.height == rhs.bounds.height && lhs.children == rhs.children &&
           lhs.child_count == rhs.child_count && lhs.children_truncated == rhs.children_truncated;
}

inline BuildStats build(Node& root, StubStyles& styles, A11ySnapshot& snapshot,
                        const BuildOptions& options = {}) {
    return tessera::ui::a11y::build_a11y_tree(root, styles, snapshot, options);
}

bool role_precedence();
bool name_precedence();
bool hidden_rules();
bool scroll_clipped_out();
bool focused_uniqueness();
bool actions_and_states();
bool preorder_and_children();
bool capacity_and_truncated_children();
bool determinism_and_automation_ids();
bool combined_scroll_focus_metadata();

} // namespace a11y_test
