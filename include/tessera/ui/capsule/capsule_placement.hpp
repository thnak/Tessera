// SPDX-License-Identifier: MIT

#pragma once

// LiveCapsule placement (docs/specs/live-capsule-v1.md section 2, issue #224): where the capsule
// goes in the window, from the cutout rectangles, the top safe-area inset and the hinge. A pure
// function over plain rectangles; no scene, no allocation.

#include "tessera/hal/geometry.hpp"
#include "tessera/hal/window_cutout_fold.hpp"

namespace tessera::ui::capsule {

// Gap below the top safe-area inset when no cutout is used (logical pixels).
inline constexpr float kFallbackTopGap = 8.0f;

struct CapsulePlacementInput {
    float window_width = 0.0f;
    float window_height = 0.0f;
    // The top safe-area inset (status bar), logical pixels.
    float top_inset = 0.0f;
    hal::CutoutInfo cutouts;
    // Hinge rectangle; zero size when there is no fold.
    hal::WindowRect hinge;
    // The capsule box to place. The height is the box height; the width already includes the
    // cutout wrap for the compact state (see `capsule_size`).
    float width = 0.0f;
    float height = 0.0f;
};

struct CapsulePlacement {
    hal::WindowRect rect;
    // True when the capsule is centred on a cutout; false for the top-centre fallback.
    bool on_cutout = false;
    // True when a hinge forced the capsule to the larger pane.
    bool moved_for_hinge = false;
};

// The cutout the capsule centres on: the widest one that touches the top edge and does not touch a
// side edge (a landscape side cutout never anchors the capsule). Returns false when there is none.
[[nodiscard]] bool top_cutout(const hal::CutoutInfo& cutouts, float window_width,
                              hal::WindowRect& out) noexcept;

[[nodiscard]] CapsulePlacement place_capsule(const CapsulePlacementInput& input) noexcept;

} // namespace tessera::ui::capsule
