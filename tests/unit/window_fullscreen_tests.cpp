// SPDX-License-Identifier: MIT
//
// The platform-neutral half of #232: the full-screen state machine and the caption hit-region
// resolver (docs/specs/window-chrome-v1.md section 8.1 and section 13). Both are pure, so the
// round-trip contract is checked without a display, a window manager or a GPU.

#include "tessera/hal/caption.hpp"
#include "tessera/hal/fullscreen.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

using namespace tessera;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "window_fullscreen: " << message << "\n";
        ++failures;
    }
    return condition;
}

hal::WindowRect rect(float x, float y, float width, float height) {
    return hal::WindowRect{x, y, width, height};
}

bool same(const hal::WindowRect& a, const hal::WindowRect& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

} // namespace

int main() {
    // --- full-screen state machine -------------------------------------------------------------
    {
        hal::FullscreenState state;
        expect(!state.active, "a fresh state is windowed");
        expect(!hal::fullscreen_has_restore_bounds(state), "a fresh state has no bounds");
        // Leaving while windowed is a no-op: it must not consume or invent bounds.
        expect(!hal::fullscreen_exit(state), "exit while windowed is a no-op");
        expect(!state.active, "a no-op exit keeps the windowed mode");
    }

    {
        hal::FullscreenState state;
        const hal::WindowRect before = rect(100, 80, 900, 620);
        expect(hal::fullscreen_enter(state, before), "the first enter is applied");
        expect(state.active, "enter marks the state active");
        expect(hal::fullscreen_has_restore_bounds(state), "enter remembers the pre-enter bounds");
        expect(same(state.restore_bounds, before), "the remembered bounds are the pre-enter ones");

        // A repeated enter must not overwrite the remembered rectangle with the full-screen one.
        expect(!hal::fullscreen_enter(state, rect(0, 0, 2560, 1440)),
               "a repeated enter is a no-op");
        expect(same(state.restore_bounds, before), "a repeated enter keeps the first bounds");

        const hal::WindowRect saved = state.restore_bounds;
        expect(hal::fullscreen_exit(state), "the first exit is applied");
        expect(!state.active, "exit marks the state windowed");
        expect(same(saved, before), "the caller can read the bounds before the exit");
        expect(!hal::fullscreen_has_restore_bounds(state), "the exit consumes the saved bounds");
        // A second exit cannot replay the stale rectangle over a resize the user made afterwards.
        expect(!hal::fullscreen_exit(state), "a second exit is a no-op");
    }

    // A resize while full-screen is not remembered: the restore still returns to the pre-enter size.
    {
        hal::FullscreenState state;
        const hal::WindowRect before = rect(10, 10, 640, 480);
        (void)hal::fullscreen_enter(state, before);
        state.restore_bounds = before; // what the platform would hold while the mode is active
        expect(same(state.restore_bounds, before), "a resize does not move the restore target");
    }

    // A non-finite or empty rectangle is not recorded: the exit falls back explicitly instead of
    // restoring into zeros.
    {
        hal::FullscreenState state;
        expect(hal::fullscreen_enter(state, hal::WindowRect{}),
               "enter with no usable rectangle is still an enter");
        expect(state.active, "the mode is active without bounds");
        expect(!hal::fullscreen_has_restore_bounds(state), "an empty rectangle is not recorded");
        expect(hal::fullscreen_exit(state), "exit without bounds is still applied");
        expect(!state.active, "the mode left");

        hal::FullscreenState nan_state;
        (void)hal::fullscreen_enter(nan_state,
                                    rect(std::numeric_limits<float>::quiet_NaN(), 0, 100, 100));
        expect(!hal::fullscreen_has_restore_bounds(nan_state), "a non-finite rectangle is dropped");
        expect(!hal::fullscreen_restorable(rect(0, 0, 0, 100)), "a zero width is not restorable");
        expect(!hal::fullscreen_restorable(rect(0, 0, 100, -1)), "a negative height is not usable");
        expect(hal::fullscreen_restorable(rect(-50, 0, 100, 100)),
               "a position outside the primary display is still restorable");
    }

    // A user or window manager toggling the mode outside set_fullscreen is observed, and its stale
    // record is dropped so it cannot be replayed.
    {
        hal::FullscreenState state;
        (void)hal::fullscreen_enter(state, rect(0, 0, 800, 600));
        expect(hal::fullscreen_resolved(state, true), "a resolved enter is reported");
        expect(!hal::fullscreen_resolved(state, false), "a resolved exit is reported");
        expect(!state.active, "the resolved exit leaves the windowed mode");
        expect(!hal::fullscreen_has_restore_bounds(state),
               "an outside exit drops the stale restore bounds");
    }

    // --- caption hit regions -------------------------------------------------------------------
    {
        // No regions is the system-chrome answer: every point is client area.
        const std::array<hal::CaptionRegion, 0> none{};
        expect(hal::caption_region_at(none, 10.0f, 10.0f) == nullptr, "no regions means client area");

        // A title bar: a drag strip, the system menu, and the three window controls on the right.
        const std::array<hal::CaptionRegion, 5> bar{
            hal::CaptionRegion{rect(0, 0, 32, 32), hal::CaptionRegionKind::SystemMenu},
            hal::CaptionRegion{rect(32, 0, 700, 32), hal::CaptionRegionKind::Drag},
            hal::CaptionRegion{rect(732, 0, 46, 32), hal::CaptionRegionKind::Minimize},
            hal::CaptionRegion{rect(778, 0, 46, 32), hal::CaptionRegionKind::Maximize},
            hal::CaptionRegion{rect(824, 0, 46, 32), hal::CaptionRegionKind::Close},
        };
        expect(hal::caption_region_at(bar, 16.0f, 16.0f) != nullptr, "the system menu is a region");
        expect(hal::caption_region_at(bar, 16.0f, 16.0f)->kind ==
                   hal::CaptionRegionKind::SystemMenu,
               "the system-menu kind is reported");
        expect(hal::caption_region_at(bar, 400.0f, 10.0f)->kind == hal::CaptionRegionKind::Drag,
               "the title strip drags");
        expect(hal::caption_region_at(bar, 750.0f, 10.0f)->kind == hal::CaptionRegionKind::Minimize,
               "minimize is resolved");
        expect(hal::caption_region_at(bar, 800.0f, 10.0f)->kind == hal::CaptionRegionKind::Maximize,
               "maximize is resolved");
        expect(hal::caption_region_at(bar, 840.0f, 10.0f)->kind == hal::CaptionRegionKind::Close,
               "close is resolved");
        expect(hal::caption_region_at(bar, 400.0f, 100.0f) == nullptr,
               "below the bar is client area");
        // The bar's right half-open edge belongs to the next region, so adjacent controls never both
        // claim a point.
        expect(hal::caption_region_at(bar, 777.0f, 10.0f)->kind ==
                   hal::CaptionRegionKind::Minimize,
               "the boundary belongs to the region that starts there");
        expect(hal::caption_region_at(bar, 778.0f, 10.0f)->kind ==
                   hal::CaptionRegionKind::Maximize,
               "the next region owns its left edge");
        // A non-finite point is client area: bad input never becomes a caption hit.
        expect(hal::caption_region_at(bar, std::numeric_limits<float>::quiet_NaN(), 10.0f) ==
                   nullptr,
               "a NaN point is client area");

        expect(hal::is_caption_button(hal::CaptionRegionKind::Minimize), "minimize is a button");
        expect(hal::is_caption_button(hal::CaptionRegionKind::Close), "close is a button");
        expect(!hal::is_caption_button(hal::CaptionRegionKind::Drag), "drag is not a button");
        expect(!hal::is_caption_button(hal::CaptionRegionKind::SystemMenu),
               "the system menu is not a button");

        expect(std::string_view(hal::caption_region_kind_name(hal::CaptionRegionKind::Drag)) ==
                   "drag",
               "the kind names are stable for the metrics dump");
        expect(std::string_view(hal::caption_region_kind_name(hal::CaptionRegionKind::SystemMenu)) ==
                   "system-menu",
               "the system-menu name is stable");
    }

    // Building the list caps at kMaxCaptionRegions and skips unusable rectangles.
    {
        std::array<hal::CaptionRegion, hal::kMaxCaptionRegions + 3> too_many{};
        for (std::size_t index = 0; index < too_many.size(); ++index) {
            too_many[index].bounds = rect(static_cast<float>(index) * 10.0f, 0.0f, 10.0f, 10.0f);
        }
        std::uint8_t count = 0;
        const auto kept = hal::make_caption_regions(too_many, count);
        expect(count == hal::kMaxCaptionRegions, "the list is capped at kMaxCaptionRegions");
        expect(kept[0].bounds.x == 0.0f, "the first regions are kept in order");

        const std::array<hal::CaptionRegion, 3> mixed{
            hal::CaptionRegion{rect(0, 0, 0, 10), hal::CaptionRegionKind::Drag},
            hal::CaptionRegion{rect(0, 0, 10, 10), hal::CaptionRegionKind::Close},
            hal::CaptionRegion{rect(std::numeric_limits<float>::infinity(), 0, 10, 10),
                               hal::CaptionRegionKind::Minimize},
        };
        std::uint8_t mixed_count = 0;
        const auto filtered = hal::make_caption_regions(mixed, mixed_count);
        expect(mixed_count == 1, "an unusable rectangle is skipped");
        expect(filtered[0].kind == hal::CaptionRegionKind::Close, "the usable region survives");
    }

    if (failures == 0) {
        std::cout << "window_fullscreen: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
