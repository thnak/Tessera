// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/node_types.hpp"

#include <cstdint>

// The scroll *configuration* of a scene node, packed into one byte: which axes scroll and the three
// resolved style inputs (`scroll-behavior`, `overscroll-behavior`, `overscroll-effect`;
// docs/specs/stylesheet-v1.md section 11 and docs/specs/scroll-and-gestures-v1.md section 4). It is
// the hot half of the scroll state: every hit-test visit asks `scrollable_x/y`, so it sits on the
// node's first cache line, while the ten floats of offset and insets (`NodeScroll`) are on demand.
//
// The enums are stored in declaration order and the masks are sized to the largest enumerator.
// Defaults: not scrollable, `scroll-behavior: auto`, `overscroll-behavior: auto` and the registry's
// initial `overscroll-effect: platform`. Setters return whether the value changed; what a change
// invalidates is the owner's decision.
namespace tessera::ui {

class NodeScrollStyle {
public:
    [[nodiscard]] bool scrollable_x() const noexcept {
        return (bits_ & kScrollableX) != 0;
    }
    bool set_scrollable_x(bool on) noexcept {
        return write(kScrollableX, on ? kScrollableX : 0u);
    }
    [[nodiscard]] bool scrollable_y() const noexcept {
        return (bits_ & kScrollableY) != 0;
    }
    bool set_scrollable_y(bool on) noexcept {
        return write(kScrollableY, on ? kScrollableY : 0u);
    }
    [[nodiscard]] bool scrollable() const noexcept {
        return (bits_ & (kScrollableX | kScrollableY)) != 0;
    }

    [[nodiscard]] ScrollBehavior scroll_behavior() const noexcept {
        return (bits_ & kSmooth) != 0 ? ScrollBehavior::Smooth : ScrollBehavior::Auto;
    }
    bool set_scroll_behavior(ScrollBehavior value) noexcept {
        return write(kSmooth, value == ScrollBehavior::Smooth ? kSmooth : 0u);
    }
    [[nodiscard]] OverscrollBehavior overscroll_behavior() const noexcept {
        return static_cast<OverscrollBehavior>((bits_ & kBehaviorMask) >> kBehaviorShift);
    }
    bool set_overscroll_behavior(OverscrollBehavior value) noexcept {
        return write(kBehaviorMask, (static_cast<unsigned>(value) << kBehaviorShift) & kBehaviorMask);
    }
    [[nodiscard]] OverscrollEffect overscroll_effect() const noexcept {
        return static_cast<OverscrollEffect>((bits_ & kEffectMask) >> kEffectShift);
    }
    bool set_overscroll_effect(OverscrollEffect value) noexcept {
        return write(kEffectMask, (static_cast<unsigned>(value) << kEffectShift) & kEffectMask);
    }

private:
    static constexpr unsigned kScrollableX = 1u << 0;
    static constexpr unsigned kScrollableY = 1u << 1;
    static constexpr unsigned kSmooth = 1u << 2;
    static constexpr unsigned kBehaviorShift = 3;
    static constexpr unsigned kBehaviorMask = 0x3u << kBehaviorShift;
    static constexpr unsigned kEffectShift = 5;
    static constexpr unsigned kEffectMask = 0x7u << kEffectShift;
    static_assert(static_cast<unsigned>(OverscrollBehavior::None) == 2,
                  "OverscrollBehavior is packed in declaration order");
    static_assert(static_cast<unsigned>(OverscrollEffect::Platform) == 4,
                  "OverscrollEffect is packed in declaration order");

    // Replaces the bits under `mask` with `value`; true when that changed the byte.
    bool write(unsigned mask, unsigned value) noexcept {
        const auto next = static_cast<std::uint8_t>((bits_ & ~mask) | value);
        const bool changed = next != bits_;
        bits_ = next;
        return changed;
    }

    std::uint8_t bits_ = static_cast<std::uint8_t>(static_cast<unsigned>(OverscrollEffect::Platform)
                                                   << kEffectShift);
};

static_assert(sizeof(NodeScrollStyle) == 1, "NodeScrollStyle is one byte on the first line");

} // namespace tessera::ui
