// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>

// The one rule that decides whether a canvas viewport request is legal (issue #690).
//
// **Why this is its own translation unit.** It used to live beside the other ABI argument parsing
// in `wasm_canvas_request.cpp`, which pulls in the font stack (`canvas_measurer`) through the
// session source. The limits are the part a host lane can test without a GPU, a device or a font,
// so they live where a test can link them alone - the same split `wasm_canvas_safe_area.cpp` and
// `wasm_canvas_support.cpp` make for their own pure rules.
//
// Both sides of the ABI apply this rule: the module does (below), and the page does through
// `clampViewport` in `designer/web/src/views/canvas-viewport.ts`. The two are kept in step by
// `tests/unit/canvas_viewport_limit_tests.cpp` (the engine) and
// `scripts/ci/preview_viewport_limits.mjs` (the page), which assert the same numbers.
namespace tessera::compiler::codegen::cli {

// One `tessera_canvas_set_viewport` request (issue #436): the logical rectangle and the DPR, or the
// reason it was refused. A non-positive extent is an error rather than a fallback, because laying a
// tree out at a degenerate rectangle and then reporting success would show the next frame at a size
// nobody asked for.
struct CanvasViewportRequest {
    float width = 0.0f;
    float height = 0.0f;
    float dpi = 1.0f;
    std::string error; // empty when the request is usable
    // True when a usable request was reduced to fit (issue #690): the caller asked for more than
    // the limits below allow, and got the largest legal rectangle instead of a refusal.
    bool clamped = false;
};

// The viewport limits every canvas request is held to (issue #690).
//
// **Why they exist.** `canvas_viewport_request` used to refuse only non-positive values, so a typed
// size or a device profile at a high DPR could ask the canvas for a backing store larger than
// WebGPU's own limit for a render target, or than any real texture memory. That is the failure the
// limits prevent: surface creation fails or the device is lost, and the preview is gone rather than
// merely clamped.
//
// **What they are.** The *page's* limits, deliberately conservative and not the device's own
// `maxTextureDimension2D`: the template module has no device when it parses a viewport, and asking
// the device would make "is this size legal" depend on hardware. Every WebGPU implementation
// reports a canvas maximum of at least 8192, and the memory ceiling keeps the backing store
// (extent x DPR, 4 bytes a pixel) at or under 256 MiB.
//
// A value over a limit is **clamped, not refused**: a clamp names the size it applied, while a
// refusal would leave the page believing it had asked for something the engine then silently
// dropped. A value that is not a number, or is not positive, is still a refusal - it names no size
// at all.
inline constexpr float kViewportMinExtent = 1.0f;
inline constexpr float kViewportMaxExtent = 8192.0f;
inline constexpr float kViewportMinDpr = 0.01f;
inline constexpr float kViewportMaxDpr = 4.0f;
inline constexpr std::uint64_t kViewportMaxBackingBytes = 256ULL * 1024ULL * 1024ULL;

[[nodiscard]] CanvasViewportRequest canvas_viewport_request(float width, float height, float dpi);

} // namespace tessera::compiler::codegen::cli
