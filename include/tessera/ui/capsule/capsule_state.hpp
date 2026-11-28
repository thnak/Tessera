// SPDX-License-Identifier: MIT

#pragma once

// LiveCapsule states, sizing and transitions (docs/specs/live-capsule-v1.md section 3 and 5, issue
// #224). The controller is pure state: it holds no node and no clock, so the headless tests drive
// it directly and the component maps it to nodes.

#include <cstdint>

namespace tessera::ui::capsule {

enum class CapsuleState : std::uint8_t { Minimal, Compact, Expanded };

inline constexpr float kCompactMaxWidthFraction = 0.6f;
inline constexpr float kExpandedMaxWidthFraction = 0.9f;
inline constexpr float kExpandedMaxHeightFraction = 0.4f;
inline constexpr float kMinimalDotWidth = 24.0f;
inline constexpr float kCapsuleHeight = 36.0f;

struct CapsuleSizeInput {
    CapsuleState state = CapsuleState::Compact;
    float window_width = 0.0f;
    float window_height = 0.0f;
    // Width of the cutout the capsule wraps, 0 when there is none.
    float cutout_width = 0.0f;
    float cutout_height = 0.0f;
    float leading_width = 0.0f;
    float trailing_width = 0.0f;
    // The natural size of the expanded card.
    float card_width = 0.0f;
    float card_height = 0.0f;
};

struct CapsuleSize {
    float width = 0.0f;
    float height = 0.0f;
    float corner_radius = 0.0f;
};

// The target box of a state. Minimal is at most the cutout width plus one dot; compact is the
// cutout plus twice the wider slot (the capsule is centred on the cutout; one row of both slots
// with no cutout) capped at 60% of the window; expanded is the card capped at 90% x 40%. The
// pill radius is half the height.
[[nodiscard]] CapsuleSize capsule_size(const CapsuleSizeInput& input) noexcept;

// Which transition an input asks for; the controller applies it.
class CapsuleController {
public:
    [[nodiscard]] CapsuleState state() const noexcept {
        return state_;
    }
    // The state a collapse returns to.
    [[nodiscard]] CapsuleState previous() const noexcept {
        return previous_;
    }
    // Starts in `state` (a new front activity) and forgets the saved focus.
    void reset(CapsuleState state) noexcept;

    // Tap, Enter or Space on a minimal or compact capsule. `focused_node` is the node that held
    // focus (0 for none) and is handed back by the collapse. True when the state changed.
    bool activate(std::uint64_t focused_node) noexcept;
    // Escape, or a press outside the capsule; only an expanded capsule reacts.
    bool escape() noexcept;
    bool outside_press() noexcept;
    // An upward swipe while expanded. A swipe that starts inside the system gesture inset
    // (`start_y` below `gesture_inset_top`) is ignored, so it never fights the system gesture.
    bool swipe_up(float start_y, float gesture_inset_top) noexcept;

    // The focus to restore after a collapse; 0 when none. Reading it clears it.
    [[nodiscard]] std::uint64_t take_focus_return() noexcept;

private:
    bool collapse() noexcept;

    CapsuleState state_ = CapsuleState::Compact;
    CapsuleState previous_ = CapsuleState::Compact;
    std::uint64_t focus_return_ = 0;
};

} // namespace tessera::ui::capsule
