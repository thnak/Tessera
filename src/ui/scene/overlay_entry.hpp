// SPDX-License-Identifier: MIT

#pragma once

// The private state of one overlay entry, shared by the translation units that implement
// OverlayHost (lifecycle, dismissal, placement). Not installed.

#include "tessera/ui/overlay/overlay.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace tessera::ui {

namespace overlay_detail {

// Inline style text is borrowed by the scene. A change is written into the buffer the declaration
// does not point at, so the new text is seen as a change instead of comparing equal to itself.
struct PixelText {
    std::array<std::array<char, 24>, 2> buffers{{{'0', 'p', 'x', '\0'}, {'0', 'p', 'x', '\0'}}};
    std::size_t current = 0;

    std::string_view write(float value) {
        current ^= 1U;
        std::array<char, 24>& buffer = buffers[current];
        std::snprintf(buffer.data(), buffer.size(), "%.2fpx", static_cast<double>(value));
        return buffer.data();
    }
    [[nodiscard]] std::string_view text() const {
        return buffers[current].data();
    }
};

[[nodiscard]] inline bool within(const Node* ancestor, const Node* node) noexcept {
    for (; node != nullptr && ancestor != nullptr; node = node->parent()) {
        if (node == ancestor) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline Node* find_node(Node& node, NodeId id) {
    if (node.id() == id) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (Node* found = find_node(*child, id)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace overlay_detail

struct OverlayHost::Entry {
    enum class State : std::uint8_t { Pending, Open };

    std::uint64_t sequence = 0;
    OverlaySpec spec;
    State state = State::Pending;
    bool closing = false;
    Node* wrapper = nullptr; // owned by the layer node while Open
    Node* content = nullptr; // owned by the wrapper
    overlay_detail::PixelText left;
    overlay_detail::PixelText top;
    bool placed = false; // the content has a computed position
};

} // namespace tessera::ui
