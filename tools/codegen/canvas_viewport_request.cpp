// SPDX-License-Identifier: MIT

#include "canvas_viewport_request.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::compiler::codegen::cli {

namespace {

// One axis of the logical viewport, clamped to the limits in the header. `clamped` is raised when
// the value had to move, so the report can name the size it applied rather than the one it was
// asked for.
float clamp_extent(float value, bool& clamped) {
    const float bounded = std::clamp(value, kViewportMinExtent, kViewportMaxExtent);
    clamped = clamped || bounded != value;
    return bounded;
}

// The DPR is a scale, so it has its own bounds: a `0.01` device is legal (what an unusually small
// zoom asks for), a `100` one is not.
float clamp_dpr(float value, bool& clamped) {
    const float bounded = std::clamp(value, kViewportMinDpr, kViewportMaxDpr);
    clamped = clamped || bounded != value;
    return bounded;
}

// The memory ceiling, applied after both axes and the DPR. A 4096x4096 rectangle at DPR 4 is 1 GiB
// of backing store; the scale is reduced until the product fits, which is the one clamp a caller
// cannot see in a single field. Reducing the DPR (not the extent) keeps the logical layout the
// caller asked for, so a clamp never changes what a breakpoint sees.
void clamp_backing_bytes(CanvasViewportRequest& request) {
    const double bytes = static_cast<double>(request.width) * static_cast<double>(request.height) *
                         static_cast<double>(request.dpi) * static_cast<double>(request.dpi) * 4.0;
    const double cap = static_cast<double>(kViewportMaxBackingBytes);
    if (bytes <= cap) {
        return;
    }
    const double scale = std::sqrt(cap / bytes);
    request.dpi = std::min(static_cast<float>(request.dpi * scale), kViewportMaxDpr);
    request.clamped = true;
}

} // namespace

CanvasViewportRequest canvas_viewport_request(float width, float height, float dpi) {
    CanvasViewportRequest request;
    // A non-finite or non-positive value names no device at all: refused, never clamped, because
    // "the largest legal size" is an answer to a question the caller did not ask. `std::isfinite`
    // is also what keeps a NaN out of `std::clamp`, whose comparison behaviour on a NaN is
    // undefined.
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0f || height <= 0.0f) {
        // Refused, not defaulted: a toolbar that sent 0 would otherwise lay the tree out at the
        // module's 320x240 default while the page believed it had asked for something else.
        request.error = "the viewport must be positive in both dimensions";
        return request;
    }
    if (!std::isfinite(dpi) || dpi <= 0.0f) {
        request.error = "the device pixel ratio must be positive";
        return request;
    }
    request.width = clamp_extent(width, request.clamped);
    request.height = clamp_extent(height, request.clamped);
    request.dpi = clamp_dpr(dpi, request.clamped);
    clamp_backing_bytes(request);
    return request;
}

} // namespace tessera::compiler::codegen::cli
