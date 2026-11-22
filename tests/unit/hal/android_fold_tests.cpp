// SPDX-License-Identifier: MIT
//
// Fold decoding (issue #189, docs/design/mobile-integration.md section 3.6): pure over plain
// numbers, so it runs on every lane with no Android SDK and no device.
//
// Why this is its own test file rather than more assertions in android_metrics_tests.cpp: the fold
// has a rule the other metrics do not — a flat fold is a seam, not a barrier — and that rule is the
// one most likely to be got wrong. Keeping it separate makes the failure obvious.

#include "android_metrics_translation.hpp"

#include <iostream>

namespace {

namespace android = tessera::hal::android;
using tessera::hal::FoldOcclusion;
using tessera::hal::FoldOrientation;
using tessera::hal::FoldState;

int failures = 0;

void check(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        ++failures;
    }
}

} // namespace

int main() {
    constexpr float kScale = 2.75F;

    // --- No fold ------------------------------------------------------------------------------
    // A non-foldable reports no posture; every field keeps its "no fold" default rather than a
    // zeroed hinge that would look like an occluding seam at the origin.
    {
        const tessera::hal::FoldInfo info =
            android::fold_info(android::FoldPosture::None, android::FoldAxis::None, {}, kScale);
        check(info.state == FoldState::None, "no posture is no fold");
        check(info.orientation == FoldOrientation::None, "and no orientation");
        check(info.occlusion == FoldOcclusion::None, "and no occlusion");
        check(info.hinge_bounds.width == 0.0F, "and no hinge");
    }

    // --- Flat fold ----------------------------------------------------------------------------
    // A flat fold is a seam, not a barrier: it reports the state and axis but does NOT occlude.
    // Treating it as an occluder would inset the layout by a rectangle the device does not have.
    {
        android::PhysicalRect hinge;
        hinge.left = 0;
        hinge.top = 1200;
        hinge.right = 1080;
        hinge.bottom = 1210;
        const tessera::hal::FoldInfo info = android::fold_info(
            android::FoldPosture::Flat, android::FoldAxis::Horizontal, hinge, kScale);
        check(info.state == FoldState::Flat, "a flat fold reports Flat");
        check(info.orientation == FoldOrientation::Horizontal, "and its axis");
        check(info.occlusion == FoldOcclusion::None, "a flat fold does not occlude");
        check(info.hinge_bounds.height == 0.0F, "a flat fold reports no hinge");
    }

    // --- Half-opened fold ---------------------------------------------------------------------
    // The only posture that occludes, and its hinge is reported in logical pixels.
    {
        android::PhysicalRect hinge;
        hinge.left = 0;
        hinge.top = 1200;
        hinge.right = 1080;
        hinge.bottom = 1210;
        const tessera::hal::FoldInfo info = android::fold_info(
            android::FoldPosture::HalfOpened, android::FoldAxis::Horizontal, hinge, kScale);
        check(info.state == FoldState::HalfOpened, "a half-opened fold reports HalfOpened");
        check(info.orientation == FoldOrientation::Horizontal, "with its axis");
        check(info.occlusion == FoldOcclusion::Full, "and it occludes");
        check(info.hinge_bounds.y == 436.0F, "the hinge y is logical");
        check(info.hinge_bounds.height == 4.0F, "the hinge thickness is logical");
    }

    // --- A half-opened fold with no hinge rectangle -------------------------------------------
    // The platform can report a posture without a usable rectangle. Inventing one would inset the
    // layout by a band the device does not have, so the state is reported and the occlusion is not.
    {
        const tessera::hal::FoldInfo info = android::fold_info(
            android::FoldPosture::HalfOpened, android::FoldAxis::Vertical, {}, kScale);
        check(info.state == FoldState::HalfOpened, "the state is still reported");
        check(info.orientation == FoldOrientation::Vertical, "and the axis");
        check(info.occlusion == FoldOcclusion::None, "but a zero hinge does not occlude");
    }

    // --- A vertical half-opened fold ----------------------------------------------------------
    // The axis decides which extent the hinge takes; a vertical band is thin in x.
    {
        android::PhysicalRect hinge;
        hinge.left = 540;
        hinge.top = 0;
        hinge.right = 550;
        hinge.bottom = 2400;
        const tessera::hal::FoldInfo info = android::fold_info(
            android::FoldPosture::HalfOpened, android::FoldAxis::Vertical, hinge, kScale);
        check(info.orientation == FoldOrientation::Vertical, "a vertical fold reports Vertical");
        check(info.hinge_bounds.x == 196.0F, "the hinge x is logical");
        check(info.hinge_bounds.width == 4.0F, "the hinge is thin in x");
    }

    // --- A degenerate scale is the documented fallback, not a division by zero -----------------
    {
        android::PhysicalRect hinge;
        hinge.left = 0;
        hinge.top = 110;
        hinge.right = 100;
        hinge.bottom = 121;
        const tessera::hal::FoldInfo info = android::fold_info(
            android::FoldPosture::HalfOpened, android::FoldAxis::Horizontal, hinge, 0.0F);
        check(info.hinge_bounds.y == 110.0F, "a zero scale keeps physical pixels");
        check(info.hinge_bounds.height == 11.0F, "and so does the thickness");
    }

    if (failures != 0) {
        std::cerr << "android_fold: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "android_fold: ok\n";
    return 0;
}
