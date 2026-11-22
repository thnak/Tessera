// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

// The one-byte summary a scene node keeps on its first cache line about state that lives elsewhere
// (docs/architecture/god-type-audit-v1.md section 3.1; docs/design/cache-lines.md section 6).
//
// Every bit answers a question a whole-scene pass asks at every node - "is there a transform to
// apply", "is there a z-index to read", "does this subtree contain glass" - so the pass reads one
// byte and touches the cold member only when the answer is yes. The bits describe other members, so
// this type owns no rule: whoever mutates the member updates its bit in the same function (the role
// facades in `node_*_api.hpp`), and the layout check in `node.cpp` keeps the byte on line 0.
namespace tessera::ui {

class NodeHints {
public:
    // `transform_` of the placement is not the identity.
    [[nodiscard]] bool has_transform() const noexcept {
        return test(kHasTransform);
    }
    void set_has_transform(bool on) noexcept {
        assign(kHasTransform, on);
    }
    // `z_index_` of the placement is non-zero.
    [[nodiscard]] bool has_z_index() const noexcept {
        return test(kHasZIndex);
    }
    void set_has_z_index(bool on) noexcept {
        assign(kHasZIndex, on);
    }
    // The subtree contains a node with a `backdrop-filter` (derived; glass-v1 section 7).
    [[nodiscard]] bool glass_subtree() const noexcept {
        return test(kGlassSubtree);
    }
    void set_glass_subtree(bool on) noexcept {
        assign(kGlassSubtree, on);
    }
    // Layout proved this node's text payload stays inside the content box (issue #490).
    [[nodiscard]] bool text_fits_box() const noexcept {
        return test(kTextFitsBox);
    }
    void set_text_fits_box(bool on) noexcept {
        assign(kTextFitsBox, on);
    }
    // A list/grid item key is stored in the metadata. A bit rather than an optional there, because
    // key 0 is a legal key and an extra byte in the metadata record would pad the node past budget.
    [[nodiscard]] bool has_item_key() const noexcept {
        return test(kHasItemKey);
    }
    void set_has_item_key(bool on) noexcept {
        assign(kHasItemKey, on);
    }
    // A smooth-scroll animation is running. The target lives in `NodeScroll`.
    [[nodiscard]] bool scroll_animation_active() const noexcept {
        return test(kScrollAnimationActive);
    }
    void set_scroll_animation_active(bool on) noexcept {
        assign(kScrollAnimationActive, on);
    }

private:
    enum Bit : std::uint8_t {
        kHasTransform = 1u << 0,
        kHasZIndex = 1u << 1,
        kGlassSubtree = 1u << 2,
        kTextFitsBox = 1u << 3,
        kHasItemKey = 1u << 4,
        kScrollAnimationActive = 1u << 5,
    };
    [[nodiscard]] bool test(Bit bit) const noexcept {
        return (bits_ & bit) != 0;
    }
    void assign(Bit bit, bool on) noexcept {
        bits_ = static_cast<std::uint8_t>(on ? (bits_ | bit) : (bits_ & ~bit));
    }

    std::uint8_t bits_ = 0;
};

static_assert(sizeof(NodeHints) == 1, "NodeHints is one byte on the node's first cache line");

} // namespace tessera::ui
