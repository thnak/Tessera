// SPDX-License-Identifier: MIT

#include "tessera/hal/fold_posture.hpp"

#include <algorithm>

namespace tessera::hal {

namespace {

// Re-centres a hinge band of `thickness` on the middle of `extent`, as the whole-pixel rectangle a
// real device reports. `thickness` is clamped to `extent` so the two panes never go negative.
WindowRect centred_band(std::uint32_t extent, float thickness, bool vertical) noexcept {
    const float size = std::min(static_cast<float>(extent), thickness);
    const float start = (static_cast<float>(extent) - size) * 0.5f;
    WindowRect band;
    if (vertical) {
        band.x = start;
        band.width = size;
    } else {
        band.y = start;
        band.height = size;
    }
    return band;
}

bool same_rect(const WindowRect& a, const WindowRect& b) noexcept {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

} // namespace

std::optional<FoldPosture> parse_fold_posture(std::string_view text) noexcept {
    if (text == "flat") {
        return FoldPosture::Flat;
    }
    if (text == "book") {
        return FoldPosture::Book;
    }
    if (text == "tabletop") {
        return FoldPosture::Tabletop;
    }
    // "none" is not a posture a client asks for; it is what `fold_posture_name` reports for the
    // absence of one, and it parses here too so a round trip through the wire stays lossless.
    if (text == "none") {
        return FoldPosture::None;
    }
    return std::nullopt;
}

std::string_view fold_posture_name(FoldPosture posture) noexcept {
    switch (posture) {
    case FoldPosture::Flat:
        return "flat";
    case FoldPosture::Book:
        return "book";
    case FoldPosture::Tabletop:
        return "tabletop";
    case FoldPosture::None:
        break;
    }
    return "none";
}

FoldInfo fold_info_for(FoldPosture posture, std::uint32_t width, std::uint32_t height) noexcept {
    FoldInfo fold;
    if (width == 0 || height == 0) {
        return fold; // no extent, no axis to split
    }
    switch (posture) {
    case FoldPosture::Flat:
        // Fully open: the state a foldable reports with no crease in the window. `orientation`
        // stays `None` because there is no band to orient, and no occlusion, so
        // `resolve_safe_area` adds nothing for the hinge source.
        fold.state = FoldState::Flat;
        return fold;
    case FoldPosture::Book:
        fold.state = FoldState::HalfOpened;
        fold.orientation = FoldOrientation::Vertical;
        fold.occlusion = FoldOcclusion::Full;
        fold.separating = true;
        fold.hinge_bounds = centred_band(width, kSimulatedHingeSize, true);
        fold.hinge_bounds.height = static_cast<float>(height);
        return fold;
    case FoldPosture::Tabletop:
        fold.state = FoldState::HalfOpened;
        fold.orientation = FoldOrientation::Horizontal;
        fold.occlusion = FoldOcclusion::Full;
        fold.separating = true;
        fold.hinge_bounds = centred_band(height, kSimulatedHingeSize, false);
        fold.hinge_bounds.width = static_cast<float>(width);
        return fold;
    case FoldPosture::None:
        break;
    }
    return fold;
}

bool fold_changed(const FoldInfo& before, const FoldInfo& after) noexcept {
    return before.state != after.state || before.orientation != after.orientation ||
           before.occlusion != after.occlusion || before.separating != after.separating ||
           !same_rect(before.hinge_bounds, after.hinge_bounds);
}

FoldInfo rotate_fold(const FoldInfo& fold, std::uint32_t width, std::uint32_t height) noexcept {
    // Only a half-opened fold has an axis whose role a rotation can swap. `Flat` has no band and
    // `None` has no fold at all, so both are returned as they are: rotating them changes nothing a
    // consumer reads.
    if (fold.state != FoldState::HalfOpened || fold.orientation == FoldOrientation::None) {
        return fold;
    }
    const float thickness = fold.orientation == FoldOrientation::Vertical
                                ? fold.hinge_bounds.width
                                : fold.hinge_bounds.height;
    FoldInfo rotated = fold;
    if (fold.orientation == FoldOrientation::Vertical) {
        rotated.orientation = FoldOrientation::Horizontal;
        rotated.hinge_bounds = centred_band(height, thickness, false);
        rotated.hinge_bounds.width = static_cast<float>(width);
    } else {
        rotated.orientation = FoldOrientation::Vertical;
        rotated.hinge_bounds = centred_band(width, thickness, true);
        rotated.hinge_bounds.height = static_cast<float>(height);
    }
    return rotated;
}

} // namespace tessera::hal
