// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/surface.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace tessera::rendering {

// What one composite attempt did. Every value is a *state*, never an exception: the UI thread calls
// `begin_frame()`/`end_frame()` once per frame and reacts on a later tick.
enum class SurfaceCompositeStatus : std::uint8_t {
    // The frame was recorded and submitted asynchronously. Nothing waited.
    Composited,
    // Every frame slot is still in flight. The attempt is skipped rather than waiting on the GPU
    // (ADR-021); the caller retries on a later tick.
    SlotBusy,
    // The borrowed surface is invalid: its owner was destroyed or revoked it (see
    // `hal::NativeSurface`). Nothing was recorded.
    NoSurface,
    // The device reported Lost. Stop presenting and recover (D7); nothing was recorded.
    DeviceLost,
    // The caller's destination cannot receive this surface's composite: its extent does not match
    // the surface's physical pixels, or its format does not match the config's. A format mismatch
    // would be a silent colour change (the composite is a format-preserving copy/blit), so it is
    // refused instead of guessed.
    IncompatibleDestination,
};

// One open frame of a `SurfaceRenderer`. The caller draws into `target` through `commands`, closes
// its own render pass, and then calls `end_frame()`, which composites target -> destination.
struct SurfaceFrame {
    // Borrowed and valid until `end_frame()`.
    rhi::CommandList* commands = nullptr;
    rhi::RenderTarget* target = nullptr;
    SurfaceCompositeStatus status = SurfaceCompositeStatus::SlotBusy;

    [[nodiscard]] explicit operator bool() const noexcept {
        return commands != nullptr && target != nullptr;
    }
};

struct SurfaceRendererConfig {
    // Format of the composited target, which must equal the destination's (the composite is a
    // format-preserving copy/blit; a mismatch is rejected instead of guessed).
    rhi::TextureFormat format = rhi::TextureFormat::R8G8B8A8Unorm;
    // Clear colour the frame's pass should use. `target_desc()` hands it back with `LoadOp::Clear`,
    // so a caller that opens the pass (Batched2DRenderer, raw RHI) clears to the same colour the
    // window driver would.
    rhi::Color clear_color = rhi::rgba8(0, 0, 0, 0);
    // Command lists kept alive at once. 2 matches the window driver's double buffering; a slot is
    // handed out again only once its own list reports complete, which is a poll, never a wait.
    std::uint32_t frames_in_flight = 2;
};

// Composites into a `hal::NativeSurface` through the RHI (ADR-050 section 7, roadmap AF6, issue
// #178), in the ADR's default `offscreen` composition mode: the surface's renderer draws into its
// own target and the result is composited into the surface's presentation image.
//
// The path is identical on every backend and needs no window:
//
//   SurfaceFrame frame = renderer.begin_frame(destination);
//   // ...draw into *frame.target through *frame.commands, then close the pass...
//   renderer.end_frame();   // composite target -> destination, submit asynchronously
//
// The pass is the caller's, on purpose: that is what lets one `Batched2DRenderer` (which owns its
// pass) draw a surface with no adaptor in between, and it keeps this type out of the paint path.
// `target_desc()` carries the extent, format and clear colour to open it with.
//
// `destination` is a parameter on purpose, exactly as `webgpu_canvas_target.cpp` wraps a view the
// page owns: a native surface's presentation image is re-created by its owner (resize, out-of-date,
// device restore), and this type must never cache one. `destination` must stay alive until the
// submission that reads it completes, which the caller already guarantees by owning it for the
// frame.
//
// Nothing here waits: no `wait_idle`, no fence wait, no detached task (ADR-021). A slot that is
// still in flight is reported as `SlotBusy` and the frame is skipped.
//
// Offscreen-capable: a surface whose `SurfaceDesc::window_system` is `NoWindow` has no platform
// target, and the same renderer still composites into a caller-provided offscreen target. That is
// how the headless tests and the pixel golden verify the path without a display, and it is the same
// code the window path runs.
class SurfaceRenderer {
public:
    // Borrows `surface`; the renderer never owns it and never extends the window's lifetime. Over
    // an invalid surface the renderer is legal and inert (`valid()` false, every frame
    // `NoSurface`), so a host can construct one before a window exists.
    SurfaceRenderer(rhi::Device& device, hal::NativeSurface surface,
                    SurfaceRendererConfig config = {});
    ~SurfaceRenderer();

    SurfaceRenderer(const SurfaceRenderer&) = delete;
    SurfaceRenderer& operator=(const SurfaceRenderer&) = delete;

    // True while the device is ready and the borrowed surface is valid.
    [[nodiscard]] bool valid() const;
    // The surface's frozen description, zeroed when invalid. Never a handle.
    [[nodiscard]] hal::SurfaceDesc surface_desc() const;
    // True when the surface carries a platform presentation target, so `destination` is expected to
    // be a swapchain image. False is the headless/offscreen case.
    [[nodiscard]] bool native_presentable() const;
    [[nodiscard]] const SurfaceRendererConfig& config() const noexcept {
        return config_;
    }

    // Physical pixels of the composited target; zero when there is no surface.
    [[nodiscard]] rhi::Extent2D extent() const;
    // The descriptor to open the frame's pass with (extent, format, clear, Store).
    [[nodiscard]] rhi::RenderTargetDesc target_desc() const;

    // Adopts a surface re-borrowed after a resize, a recreate or a device restore. The composited
    // target is dropped and recreated at the new extent on the next frame; in-flight submissions
    // are untouched (their content is no longer presented, but nothing is destroyed under the GPU).
    void rebind(hal::NativeSurface surface);

    // Takes a free slot, creates the composited target at the surface's extent when needed, and
    // returns the list plus target to draw into. An invalid result carries the reason in `status`
    // and records nothing.
    [[nodiscard]] SurfaceFrame begin_frame(rhi::RenderTarget& destination);

    // Composites the frame's target into its destination and submits asynchronously. `SlotBusy`
    // when no frame is open. The caller must have closed its render pass first.
    [[nodiscard]] SurfaceCompositeStatus end_frame();

    // Frames that reached `submit_async`. `skipped_frames()` counts the attempts reported as
    // SlotBusy/NoSurface/DeviceLost/IncompatibleDestination, so a test can assert "a static surface
    // presents nothing".
    [[nodiscard]] std::uint64_t composited_frames() const {
        return composited_frames_;
    }
    [[nodiscard]] std::uint64_t skipped_frames() const {
        return skipped_frames_;
    }

private:
    [[nodiscard]] rhi::CommandList* free_slot();
    [[nodiscard]] bool prepare_target();
    void drop_target() noexcept;

    rhi::Device* device_ = nullptr;
    hal::NativeSurface surface_;
    SurfaceRendererConfig config_;
    std::unique_ptr<rhi::RenderTarget> target_;
    std::vector<std::unique_ptr<rhi::CommandList>> slots_;
    rhi::CommandList* frame_commands_ = nullptr;
    rhi::RenderTarget* frame_destination_ = nullptr;
    bool in_frame_ = false;
    std::uint64_t composited_frames_ = 0;
    std::uint64_t skipped_frames_ = 0;
};

} // namespace tessera::rendering
