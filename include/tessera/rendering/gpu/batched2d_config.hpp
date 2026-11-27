// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/glass_blur.hpp"

#include <cstdint>

namespace tessera::rendering::gpu {

// Construction-time policy of Batched2DRenderer (batched2d_renderer.hpp), split out so the renderer
// header stays inside its size budget.
struct Batched2DConfig {
    std::uint32_t initial_vertex_capacity = 4096;
    bool enable_aa = true;
    bool golden_mode = false;
    // MSAA opt-in: 1 (default) or 4. Must equal the render target's sample count and the pipeline's
    // sample count; the renderer rejects a mismatch instead of rendering with the wrong pipeline.
    // A 4x target resolves into its 1-sample image at end of pass; the batched2d path itself is
    // sample-count agnostic (SDF rounding and coverage text keep working unchanged).
    std::uint32_t sample_count = 1;

    // Number of frames whose vertex data may be alive on the GPU at once. Each frame writes into
    // its own buffer slot, used round-robin. With the default of 1 the caller finishes a frame
    // (blocking submit() or waiting on the list) before beginning the next. With N > 1 the caller
    // must not begin frame k + N until frame k's command list is complete, because that frame
    // reuses frame k's slot.
    std::uint32_t frames_in_flight = 1;

    // Bounded offscreen targets for the isolated-layer seam (offscreen-layers.md): glass-v1
    // `glassMaxBlurTargets`, 4 by default, 0 disables the pool. The cap is on targets LIVE at
    // once, not on layers: a finished layer, mask or scratch target is recycled inside the frame,
    // and a target is recycled across frames after frames_in_flight (section 5e).
    std::uint32_t max_offscreen_targets = 4;
    // Hard cap on the bytes of all pooled targets together (the RHI reports no heap budget, so it
    // is fixed): 256 MiB holds 7 frame-size targets at 4K and every count cap at 1080p. 0 = no cap.
    std::uint64_t max_offscreen_bytes = 256ull * 1024 * 1024;
    // A pooled target unused for this many frames is destroyed (never sooner than
    // frames_in_flight), so a scene that stops using layers gives the memory back. 0 never trims.
    std::uint32_t offscreen_idle_trim_frames = 120;
    // Retained layers (offscreen-layers-retention.md): the finished pixels of an unchanged
    // masked, filtered or faded group are kept so a later frame composites them instead of drawing
    // the subtree again. At most this many entries (0 disables), within this many bytes, which are
    // charged to `max_offscreen_bytes` too. A retained layer never takes a live layer's share.
    std::uint32_t max_retained_layers = 32;
    std::uint64_t max_retained_bytes = 64ull * 1024 * 1024;

    // Glass cost limits (glass-v1 sections 5 and 8), the same runtime keys the paint pass uses:
    // `glassMaxBlurRadius` (CSS px) and `glassMaxBackdropArea` (the half-resolution threshold).
    GlassLimits glass{};

    // CPU culling (issue #474): a quad whose transformed bounds cannot touch the active scissor or
    // the frame's damage regions is never staged, so its vertices are neither built nor uploaded.
    // The pixels are identical either way; the switch exists for debugging. On by default.
    bool cull_draws = true;

    // Per-fragment gradients (ADR-076). True (the default) lets a gradient layer be one quad when
    // the device and the frame pipeline support it; false forces the banded fallback on every
    // device, so a test or a measurement can A/B the two paths and exercise the fallback on
    // hardware that would never need it. The pixels differ only where the bands approximate the
    // ramp.
    bool analytic_gradients = true;
};

} // namespace tessera::rendering::gpu
