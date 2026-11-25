// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/geometry.hpp"
#include "tessera/hal/window_desc.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

// Split from window.hpp to keep that header inside its size budget; window.hpp includes it, so
// existing includers see no change.
namespace tessera::hal {

// Display cutouts (notch, punch-hole, camera island): insets say how much to avoid, the rectangles
// say where (docs/specs/os-events-v1.md section 2). `count == 0` is every platform that reports
// none; entries past `count` are zero. Build one with `make_cutout_info`.
inline constexpr std::size_t kMaxCutouts = 4;

struct CutoutInfo {
    std::array<WindowRect, kMaxCutouts> bounds{};
    std::uint8_t count = 0;
    // Curved-edge (waterfall) insets, zero when the display has none.
    WindowInsets waterfall_insets;
};

// Fold and hinge of a foldable display (docs/specs/os-events-v1.md section 2). The default is "no
// fold": every platform without a foldable reports it.
enum class FoldState : std::uint8_t { None, Flat, HalfOpened };
enum class FoldOrientation : std::uint8_t { None, Horizontal, Vertical };
enum class FoldOcclusion : std::uint8_t { None, Full };

struct FoldInfo {
    FoldState state = FoldState::None;
    FoldOrientation orientation = FoldOrientation::None;
    FoldOcclusion occlusion = FoldOcclusion::None;
    bool separating = false;
    // Hinge rectangle, logical pixels, window coordinates; zero when there is no fold. The angle
    // (degrees from the platform's hinge sensor, `kUnknownHingeAngle` when nothing measured one) is
    // independent of `state`, so a flat fold still has an angle.
    WindowRect hinge_bounds;
    float hinge_angle_degrees = kUnknownHingeAngle;
};

} // namespace tessera::hal
