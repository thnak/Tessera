// SPDX-License-Identifier: MIT

#include "wasm_canvas_safe_area.hpp"

#include <cmath>
#include <string>

namespace tessera::compiler::codegen::cli {
namespace {

// One inset side, or the reason it is not usable. Zero is the ordinary desktop answer; a negative
// or non-finite value is a manifest bug the page must hear about rather than a padding of zero.
bool usable_inset(float value, std::string& error) {
    if (!std::isfinite(value)) {
        error = "TESSERA_STUDIO_SKIN: a safe-area inset is not a finite number";
        return false;
    }
    if (value < 0.0f) {
        error = "TESSERA_STUDIO_SKIN: a safe-area inset is negative";
        return false;
    }
    return true;
}

} // namespace

CanvasSafeAreaRequest canvas_safe_area_request(float inset_left, float inset_top, float inset_right,
                                               float inset_bottom, float cutout_x, float cutout_y,
                                               float cutout_width, float cutout_height) {
    CanvasSafeAreaRequest request;
    if (!usable_inset(inset_left, request.error) || !usable_inset(inset_top, request.error) ||
        !usable_inset(inset_right, request.error) || !usable_inset(inset_bottom, request.error)) {
        return request;
    }
    request.insets = {inset_left, inset_top, inset_right, inset_bottom};
    // A cutout with no area is not a cutout: the page sends zeros for "none", and a zero rectangle
    // would otherwise sit in the corner and inset nothing while claiming to be a notch.
    const bool has_cutout = std::isfinite(cutout_width) && std::isfinite(cutout_height) &&
                            cutout_width > 0.0f && cutout_height > 0.0f;
    if (!has_cutout) {
        return request;
    }
    if (!std::isfinite(cutout_x) || !std::isfinite(cutout_y)) {
        request.error = "TESSERA_STUDIO_SKIN: a cutout position is not a finite number";
        return request;
    }
    // Built here rather than through `hal::make_cutout_info`: that helper lives in
    // `tessera_hal_window`, which carries the Win32/X11/Wayland backends, and the Wasm viewer must
    // not link an OS window backend (ADR-020's dependency direction, and the reason
    // `tessera_hal_metrics` exists). One rectangle is all a skin manifest can declare, so the
    // cap-and-drop rule that helper implements has nothing to do here.
    request.cutouts.bounds[0] = {cutout_x, cutout_y, cutout_width, cutout_height};
    request.cutouts.count = 1;
    return request;
}

} // namespace tessera::compiler::codegen::cli
