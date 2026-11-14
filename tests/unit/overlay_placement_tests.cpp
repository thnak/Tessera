// SPDX-License-Identifier: MIT
//
// Overlay placement math (issue #302, docs/specs/overlays-and-layers-v1.md section 3): sides,
// alignment, flip, shift, RTL, a window anchor, and hostile inputs. Pure: no scene.

#include "tessera/ui/overlay/placement.hpp"

#include <cmath>
#include <iostream>
#include <limits>

namespace {

using tessera::rendering::rhi::Rect;
using tessera::ui::compute_placement;
using tessera::ui::OverlayAlign;
using tessera::ui::OverlayAnchor;
using tessera::ui::OverlayPlacement;
using tessera::ui::OverlaySide;
using tessera::ui::PlacementInput;
using tessera::ui::PlacementResult;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "overlay_placement: " << message << "\n";
        ++failures;
    }
    return condition;
}

bool near(float a, float b) {
    return std::fabs(a - b) < 0.01f;
}

bool at(const PlacementResult& result, float x, float y) {
    return near(result.rect.x, x) && near(result.rect.y, y);
}

// A 400x800 window, a 100x40 trigger at (150, 300), and a 120x60 popover.
PlacementInput base(OverlaySide side) {
    PlacementInput input;
    input.anchor = {150.0f, 300.0f, 100.0f, 40.0f};
    input.viewport = {0.0f, 0.0f, 400.0f, 800.0f};
    input.width = 120.0f;
    input.height = 60.0f;
    input.placement.side = side;
    return input;
}

void test_sides_and_alignment() {
    auto result = compute_placement(base(OverlaySide::Bottom));
    expect(at(result, 140.0f, 340.0f), "below the trigger, centred on it");
    expect(result.side == OverlaySide::Bottom && !result.flipped && !result.shifted,
           "nothing needed adjusting");

    result = compute_placement(base(OverlaySide::Top));
    expect(at(result, 140.0f, 240.0f), "above the trigger");

    auto input = base(OverlaySide::Bottom);
    input.placement.gap = 8.0f;
    expect(at(compute_placement(input), 140.0f, 348.0f), "the gap separates the two");

    input.placement.align = OverlayAlign::Start;
    expect(at(compute_placement(input), 150.0f, 348.0f), "start aligns the left edges");
    input.placement.align = OverlayAlign::End;
    expect(at(compute_placement(input), 130.0f, 348.0f), "end aligns the right edges");

    result = compute_placement(base(OverlaySide::End));
    expect(at(result, 250.0f, 290.0f), "end puts it to the right, centred vertically");
    result = compute_placement(base(OverlaySide::Start));
    expect(at(result, 30.0f, 290.0f), "start puts it to the left");

    input = base(OverlaySide::Center);
    expect(at(compute_placement(input), 140.0f, 290.0f), "centre sits over the anchor");
}

void test_rtl() {
    auto input = base(OverlaySide::End);
    input.rtl = true;
    auto result = compute_placement(input);
    expect(at(result, 30.0f, 290.0f), "end is the left side under RTL");
    expect(result.side == OverlaySide::End, "and still reports the logical side");

    input = base(OverlaySide::Bottom);
    input.rtl = true;
    input.placement.align = OverlayAlign::Start;
    expect(at(compute_placement(input), 130.0f, 340.0f), "start aligns the right edges under RTL");
}

void test_flip() {
    auto input = base(OverlaySide::Bottom);
    input.anchor = {150.0f, 760.0f, 100.0f, 30.0f};
    auto result = compute_placement(input);
    expect(result.flipped && result.side == OverlaySide::Top && at(result, 140.0f, 700.0f),
           "no room below: it flips above");

    input.placement.flip = false;
    result = compute_placement(input);
    expect(!result.flipped && result.shifted && near(result.rect.y, 740.0f),
           "without flip it only shifts back inside");

    input = base(OverlaySide::Top);
    input.anchor = {150.0f, 20.0f, 100.0f, 30.0f};
    result = compute_placement(input);
    expect(result.flipped && at(result, 140.0f, 50.0f), "no room above: it flips below");

    // Neither side fits a 700-high box: the roomier side wins and the box is shifted inside.
    input = base(OverlaySide::Top);
    input.anchor = {150.0f, 200.0f, 100.0f, 40.0f};
    input.height = 700.0f;
    result = compute_placement(input);
    expect(result.flipped && result.side == OverlaySide::Bottom,
           "when both overflow, the side with more room wins");
    expect(result.rect.y >= 0.0f && result.rect.y + 700.0f <= 800.0f, "and it ends inside");

    input = base(OverlaySide::End);
    input.anchor = {330.0f, 300.0f, 60.0f, 40.0f};
    result = compute_placement(input);
    expect(result.flipped && result.side == OverlaySide::Start, "no room to the right: flips left");
}

void test_shift_and_oversize() {
    auto input = base(OverlaySide::Bottom);
    input.anchor = {350.0f, 300.0f, 40.0f, 40.0f};
    auto result = compute_placement(input);
    expect(result.shifted && near(result.rect.x, 280.0f), "slides left to stay inside");
    input.anchor = {-10.0f, 300.0f, 40.0f, 40.0f};
    result = compute_placement(input);
    expect(near(result.rect.x, 0.0f), "and right from the left edge");

    input.placement.shift = false;
    result = compute_placement(input);
    expect(!result.shifted && result.rect.x < 0.0f, "without shift it may overflow");

    input = base(OverlaySide::Bottom);
    input.width = 900.0f;
    expect(near(compute_placement(input).rect.x, 0.0f),
           "a box wider than the viewport sits at its start edge");

    input = base(OverlaySide::Bottom);
    input.viewport = {0.0f, 100.0f, 400.0f, 600.0f}; // bars take 100 above and below
    input.anchor = {150.0f, 100.0f, 100.0f, 10.0f};
    input.placement.side = OverlaySide::Top;
    const auto result_top = compute_placement(input);
    expect(result_top.rect.y >= 100.0f, "the safe viewport, not the window, is the limit");
}

void test_window_anchor() {
    PlacementInput input;
    input.anchor_is_window = true;
    input.viewport = {0.0f, 44.0f, 400.0f, 756.0f}; // a status bar above
    input.width = 160.0f;
    input.height = 36.0f;
    input.placement.anchor = OverlayAnchor::window();
    input.placement.side = OverlaySide::Top;
    input.placement.gap = 8.0f;
    auto result = compute_placement(input);
    expect(at(result, 120.0f, 52.0f), "top-centre inside the window, below the safe inset");
    expect(!result.flipped, "a window anchor never flips");

    input.placement.side = OverlaySide::Bottom;
    expect(at(compute_placement(input), 120.0f, 756.0f), "bottom-centre, a gap above the edge");

    input.placement.side = OverlaySide::Center;
    expect(at(compute_placement(input), 120.0f, 404.0f), "centred in the viewport");

    input.placement.side = OverlaySide::Start;
    input.placement.gap = 0.0f;
    expect(at(compute_placement(input), 0.0f, 404.0f), "start edge, centred vertically");
    input.rtl = true;
    expect(at(compute_placement(input), 240.0f, 404.0f), "the start edge is the right under RTL");

    input.rtl = false;
    input.placement.side = OverlaySide::Top;
    input.placement.align = OverlayAlign::End;
    expect(at(compute_placement(input), 240.0f, 44.0f), "end alignment hugs the far corner");
}

void test_hostile_inputs() {
    constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
    constexpr float kInf = std::numeric_limits<float>::infinity();
    auto input = base(OverlaySide::Bottom);
    input.anchor = {kNan, kInf, -5.0f, kNan};
    input.width = kNan;
    input.height = -3.0f;
    input.placement.gap = kInf;
    const auto result = compute_placement(input);
    expect(std::isfinite(result.rect.x) && std::isfinite(result.rect.y) &&
               std::isfinite(result.rect.width) && std::isfinite(result.rect.height),
           "non-finite input still yields a finite rectangle");
    expect(result.rect.width == 0.0f && result.rect.height == 0.0f, "negative sizes count as zero");
}

} // namespace

int main() {
    test_sides_and_alignment();
    test_rtl();
    test_flip();
    test_shift_and_oversize();
    test_window_anchor();
    test_hostile_inputs();
    return failures == 0 ? 0 : 1;
}
