// SPDX-License-Identifier: MIT

#include "tessera/ui/capsule/capsule_state.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::ui::capsule {

namespace {

float non_negative(float value) noexcept {
    return std::isfinite(value) && value > 0.0f ? value : 0.0f;
}

} // namespace

CapsuleSize capsule_size(const CapsuleSizeInput& input) noexcept {
    const float window_w = non_negative(input.window_width);
    const float window_h = non_negative(input.window_height);
    const float cutout_w = non_negative(input.cutout_width);
    CapsuleSize size;
    switch (input.state) {
    case CapsuleState::Minimal:
        size.width = cutout_w + kMinimalDotWidth;
        size.height = std::max(kCapsuleHeight, non_negative(input.cutout_height));
        size.corner_radius = size.height * 0.5f;
        break;
    case CapsuleState::Compact:
        size.width = std::min(cutout_w + non_negative(input.leading_width) +
                                  non_negative(input.trailing_width),
                              window_w * kCompactMaxWidthFraction);
        size.height = std::max(kCapsuleHeight, non_negative(input.cutout_height));
        size.corner_radius = size.height * 0.5f;
        break;
    case CapsuleState::Expanded:
        size.width = std::min(non_negative(input.card_width), window_w * kExpandedMaxWidthFraction);
        size.height =
            std::min(non_negative(input.card_height), window_h * kExpandedMaxHeightFraction);
        // The card keeps rounded ends but is not a full pill once it is taller than a row.
        size.corner_radius = std::min(size.height * 0.5f, kCapsuleHeight * 0.5f + 4.0f);
        break;
    }
    return size;
}

void CapsuleController::reset(CapsuleState state) noexcept {
    state_ = state;
    previous_ = state;
    focus_return_ = 0;
}

bool CapsuleController::activate(std::uint64_t focused_node) noexcept {
    if (state_ == CapsuleState::Expanded) {
        return false;
    }
    previous_ = state_;
    state_ = CapsuleState::Expanded;
    focus_return_ = focused_node;
    return true;
}

bool CapsuleController::collapse() noexcept {
    if (state_ != CapsuleState::Expanded) {
        return false;
    }
    state_ = previous_;
    return true;
}

bool CapsuleController::escape() noexcept {
    return collapse();
}

bool CapsuleController::outside_press() noexcept {
    return collapse();
}

bool CapsuleController::swipe_up(float start_y, float gesture_inset_top) noexcept {
    if (start_y < non_negative(gesture_inset_top)) {
        return false;
    }
    return collapse();
}

std::uint64_t CapsuleController::take_focus_return() noexcept {
    const std::uint64_t node = focus_return_;
    focus_return_ = 0;
    return node;
}

} // namespace tessera::ui::capsule
