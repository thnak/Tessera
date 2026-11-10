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
#include <iostream>
#include <string_view>

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

} // namespace a11y_uia_test
