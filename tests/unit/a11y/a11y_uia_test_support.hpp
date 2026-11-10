// SPDX-License-Identifier: MIT
//
// Shared assertions and snapshot builders for the Windows UI Automation bridge tests. Every
// executable in this directory links only `tessera_accessibility`; the UIA tests never create a
// real client and never need a desktop session.

#pragma once

#include "tessera/ui/accessibility/a11y_tree.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

namespace a11y_uia_test {

inline bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "a11y uia: " << message << "\n";
    }
    return condition;
}

inline bool expect_equal(std::string_view actual, std::string_view expected, const char* message) {
    if (actual == expected) {
        return true;
    }
    std::cerr << "a11y uia: " << message << " (actual=\"" << actual << "\", expected=\"" << expected
              << "\")\n";
    return false;
}

inline bool expect_eq(std::int64_t actual, std::int64_t expected, const char* message) {
    if (actual == expected) {
        return true;
    }
    std::cerr << "a11y uia: " << message << " (actual=" << actual << ", expected=" << expected
              << ")\n";
    return false;
}

inline void set_text(std::array<char, tessera::ui::a11y::kMaxTextBytes>& target,
                     std::string_view text) {
    const std::size_t size = std::min(text.size(), tessera::ui::a11y::kMaxTextBytes - 1U);
    std::copy_n(text.begin(), size, target.begin());
    target[size] = '\0';
}

inline tessera::ui::a11y::A11yNode
make_node(tessera::ui::a11y::Role role, std::string_view name = {}, std::string_view value = {}) {
    using namespace tessera::ui::a11y;
    A11yNode node;
    node.role = role;
    set_text(node.name, name);
    set_text(node.value, value);
    return node;
}

inline void link_children(tessera::ui::a11y::A11yNode& parent,
                          std::initializer_list<tessera::ui::a11y::A11yIndex> children) {
    std::size_t index = 0;
    for (const tessera::ui::a11y::A11yIndex child : children) {
        if (index >= tessera::ui::a11y::kMaxChildren) {
            break;
        }
        parent.children[index++] = child;
    }
    parent.child_count = static_cast<std::uint16_t>(index);
}

inline tessera::ui::a11y::A11ySnapshot snapshot_of(std::vector<tessera::ui::a11y::A11yNode> nodes) {
    tessera::ui::a11y::A11ySnapshot snapshot;
    snapshot.nodes = std::move(nodes);
    return snapshot;
}

// Root pane + one focusable button. The shape most provider tests need.
inline tessera::ui::a11y::A11ySnapshot button_snapshot(std::string_view button_name = "Save") {
    using namespace tessera::ui::a11y;
    A11yNode root = make_node(Role::Generic, "Root");
    A11yNode button = make_node(Role::Button, button_name);
    button.actions = Action::Press | Action::Focus;
    button.states = State::Focusable;
    link_children(root, {1});
    return snapshot_of({root, button});
}

} // namespace a11y_uia_test
