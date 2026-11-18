// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/window.hpp"

#include <string>

// The skin's safe area as the canvas module's ABI receives it (issue #171's skins slice,
// `designer-studio-v1.md` sections 5.1 and 5.2).
//
// **Why this is its own translation unit.** It is the one part of the skin ABI that is pure over
// its arguments: no device, no session, no font, no Emscripten. Keeping it apart from
// `wasm_canvas_request.cpp` (which reaches `canvas_measurer()` and therefore the whole text stack)
// is what lets the host lanes compile and test the refusal rules without linking a GPU text stack -
// the same split `canvas_report.cpp` makes for the serializers.
namespace tessera::compiler::codegen::cli {

// One `tessera_canvas_set_safe_area` request: the insets a device would report as
// `WindowMetrics::insets` and the cutout rectangle it would report as `WindowMetrics::cutouts`, or
// the reason they were refused.
//
// **Why a sibling export and not more arguments on `set_viewport`.** The two calls answer different
// controls: `set_viewport` is the toolbar's size/DPR pair, and this is the skin picker. A skin
// change that had to go through `set_viewport` would have to re-send a size it did not change, and
// a page built against the previous module keeps calling the three-argument form - which is what
// "keep the existing arguments working" means. The engine side is one layout pass either way
// (`CanvasSceneSession::set_safe_area`), so the split costs nothing at runtime.
//
// A negative or non-finite inset is refused rather than clamped: a skin that asked for -8 px of
// padding is a manifest bug, and silently treating it as zero would pad the content for a device
// nobody described. Zero is the desktop answer and is accepted.
struct CanvasSafeAreaRequest {
    hal::WindowInsets insets;
    hal::CutoutInfo cutouts;
    std::string error; // empty when the request is usable
};

// `cutout_*` are the cutout rectangle in logical pixels, or all zero for "no cutout". A rectangle
// with no area is dropped (it is not a cutout), which is the same rule `hal::make_cutout_info`
// applies to a platform's list.
[[nodiscard]] CanvasSafeAreaRequest
canvas_safe_area_request(float inset_left, float inset_top, float inset_right, float inset_bottom,
                         float cutout_x, float cutout_y, float cutout_width, float cutout_height);

} // namespace tessera::compiler::codegen::cli
