// SPDX-License-Identifier: MIT
//
// Android window-metrics decode (issue #189, docs/design/mobile-integration.md sections 3.5/3.6).
//
// The rules are pure over plain numbers, so they run on every lane without an NDK or a device. What
// they prove is what a device lane cannot see cheaply: the physical-to-logical conversion at a
// non-1.0 density, the cutout rectangle convention, and that a flat fold does not occlude.

#include "android_metrics_translation.hpp"

#include <array>
#include <cstdint>
#include <iostream>

namespace {

namespace android = tessera::hal::android;
using tessera::hal::FoldOcclusion;
using tessera::hal::FoldOrientation;
using tessera::hal::FoldState;
using tessera::hal::Orientation;

int failures = 0;

void check(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        ++failures;
    }
}

} // namespace

int main() {
    // --- Physical to logical ----------------------------------------------------------------
    // A 24 px status bar at 2.75x is 8.7 dp, which rounds to 9. Truncating would report 8 and shift
    // every row of the layout by one pixel.
    check(android::logical_inset(24, 2.75F) == 9.0F, "24 px at 2.75x rounds to 9");
    check(android::logical_inset(24, 1.0F) == 24.0F, "1.0x is the identity");
    check(android::logical_inset(48, 2.0F) == 24.0F, "48 px at 2.0x is 24");
    // A negative or zero inset is the platform's "no inset".
    check(android::logical_inset(0, 2.0F) == 0.0F, "zero stays zero");
    check(android::logical_inset(-5, 2.0F) == 0.0F, "a negative inset is zero");
    // A non-positive scale is the documented "unknown density" fallback: report physical pixels
    // rather than divide by zero.
    check(android::logical_inset(24, 0.0F) == 24.0F, "zero scale falls back to physical");
    check(android::logical_inset(24, -1.0F) == 24.0F, "negative scale falls back to physical");

    // --- System bars ------------------------------------------------------------------------
    {
        android::PhysicalInsets physical;
        physical.left = 0;
        physical.top = 66; // a 24 dp status bar at 2.75x
        physical.right = 0;
        physical.bottom = 132; // a 48 dp navigation bar at 2.75x
        const tessera::hal::WindowInsets insets = android::system_bar_insets(physical, 2.75F);
        check(insets.top == 24.0F, "a 66 px status bar is 24 dp");
        check(insets.bottom == 48.0F, "a 132 px navigation bar is 48 dp");
        check(insets.left == 0.0F && insets.right == 0.0F, "no side insets in portrait");
    }
    // A landscape device puts the navigation bar on a side.
    {
        android::PhysicalInsets physical;
        physical.right = 132;
        const tessera::hal::WindowInsets insets = android::system_bar_insets(physical, 2.75F);
        check(insets.right == 48.0F, "a landscape navigation bar is a right inset");
        check(insets.bottom == 0.0F, "and not a bottom one");
    }

    // --- IME and gestures -------------------------------------------------------------------
    {
        android::PhysicalInsets physical;
        physical.bottom = 880; // a 320 dp keyboard at 2.75x
        check(android::ime_insets(physical, 2.75F).bottom == 320.0F, "the IME inset is carried");
        check(android::gesture_insets(physical, 2.75F).bottom == 320.0F,
              "the gesture inset is carried");
    }

    // --- Cutouts ----------------------------------------------------------------------------
    {
        // A top-centre punch-hole: 100x100 physical at the top middle of a 1080-wide screen.
        std::array<android::PhysicalRect, 1> rects{};
        rects[0].left = 490;
        rects[0].top = 0;
        rects[0].right = 590;
        rects[0].bottom = 100;
        const tessera::hal::CutoutInfo info = android::cutout_info(rects, 2.75F);
        check(info.count == 1, "one cutout is reported");
        check(info.bounds[0].x == 178.0F, "the cutout x is logical");
        check(info.bounds[0].width == 36.0F, "the cutout width is the right-left difference");
        check(info.bounds[0].height == 36.0F, "the cutout height is the bottom-top difference");
    }
    // A zero-area rectangle is not a cutout: reporting one would claim a notch that insets nothing.
    {
        std::array<android::PhysicalRect, 1> rects{};
        rects[0].left = 100;
        rects[0].right = 100;
        const tessera::hal::CutoutInfo info = android::cutout_info(rects, 2.75F);
        check(info.count == 0, "a zero-area rectangle is dropped");
    }
    // The list is capped at kMaxCutouts rather than overrunning the array.
    {
        std::array<android::PhysicalRect, 8> rects{};
        for (std::size_t i = 0; i < rects.size(); ++i) {
            rects[i].left = static_cast<std::int32_t>(i) * 10;
            rects[i].top = 0;
            rects[i].right = rects[i].left + 5;
            rects[i].bottom = 5;
        }
        const tessera::hal::CutoutInfo info = android::cutout_info(rects, 1.0F);
        check(info.count == tessera::hal::kMaxCutouts, "the cutout list is capped");
    }

    // --- Fold -------------------------------------------------------------------------------
    {
        // No fold: every field keeps its "no fold" default.
        const tessera::hal::FoldInfo info =
            android::fold_info(android::FoldPosture::None, android::FoldAxis::None, {}, 2.75F);
        check(info.state == FoldState::None, "no posture is no fold");
        check(info.occlusion == FoldOcclusion::None, "and no occlusion");
    }
    {
        // A flat fold is a seam, not a barrier: it reports the state and axis but does not occlude.
        android::PhysicalRect hinge;
        hinge.left = 0;
        hinge.top = 1200;
        hinge.right = 1080;
        hinge.bottom = 1210;
        const tessera::hal::FoldInfo info = android::fold_info(
            android::FoldPosture::Flat, android::FoldAxis::Horizontal, hinge, 2.75F);
        check(info.state == FoldState::Flat, "a flat fold reports Flat");
        check(info.orientation == FoldOrientation::Horizontal, "and its axis");
        check(info.occlusion == FoldOcclusion::None, "a flat fold does not occlude");
    }
    {
        // A half-opened fold occludes, and its hinge is reported in logical pixels.
        android::PhysicalRect hinge;
        hinge.left = 0;
        hinge.top = 1200;
        hinge.right = 1080;
        hinge.bottom = 1210;
        const tessera::hal::FoldInfo info = android::fold_info(
            android::FoldPosture::HalfOpened, android::FoldAxis::Horizontal, hinge, 2.75F);
        check(info.state == FoldState::HalfOpened, "a half-opened fold reports HalfOpened");
        check(info.occlusion == FoldOcclusion::Full, "and occludes");
        check(info.hinge_bounds.y == 436.0F, "the hinge y is logical");
        check(info.hinge_bounds.height == 4.0F, "the hinge thickness is logical");
    }
    {
        // A half-opened fold with no hinge rectangle occludes nothing: there is no barrier to
        // report, and inventing one would inset the layout by a rectangle the device does not have.
        const tessera::hal::FoldInfo info = android::fold_info(
            android::FoldPosture::HalfOpened, android::FoldAxis::Vertical, {}, 2.75F);
        check(info.state == FoldState::HalfOpened, "the state is still reported");
        check(info.occlusion == FoldOcclusion::None, "but a zero hinge does not occlude");
    }

    // --- Orientation ------------------------------------------------------------------------
    check(android::orientation_from_extent(1080, 2400) == Orientation::Portrait,
          "a tall window is portrait");
    check(android::orientation_from_extent(2400, 1080) == Orientation::Landscape,
          "a wide window is landscape");
    check(android::orientation_from_extent(1000, 1000) == Orientation::Landscape,
          "a square window is landscape");

    if (failures != 0) {
        std::cerr << "android_metrics: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "android_metrics: ok\n";
    return 0;
}
