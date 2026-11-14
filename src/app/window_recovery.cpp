// SPDX-License-Identifier: MIT
//
// Bounded device-loss recovery for the app window driver (docs/design/window-resize.md
// section 7.2).
//
// A loss is state, not an exception: the runtime's DeviceRecoveryController reports it blocked and
// owns the backend's single bounded in-place `Device::recover()`. This file owns the caller side -
// release the lost generation, recreate the device when the in-place attempt is spent, recreate the
// whole presentation stack for the window's current metrics, request a full repaint - and charges
// the explicit attempt budget in app/recovery_policy.hpp. It never waits device-wide on the UI
// thread: the only device-wide wait is the backend's own simulated-loss teardown inside recover().

#include "tessera/app/idle_policy.hpp"
#include "tessera/app/recovery_policy.hpp"
#include "window_impl.hpp"

#include <cstdio>
#include <stdexcept>
#include <utility>

namespace tessera::app {

namespace hal = tessera::hal;
namespace rhi = tessera::rendering::rhi;

bool WindowHost::Impl::create_presentation() {
    if (device == nullptr || window == nullptr) {
        return false;
    }
    const hal::WindowMetrics metrics = window->metrics();
    // The platform resolved whether the window system can present a translucent client area; the
    // frame clear follows the swapchain's actual composite mode (set below), not this flag alone.
    translucent_presentation = false;
    // The RHI probe is the only authority on the GPU frame-time signal (docs/design/performance.md
    // section 6.1). It is resolved once per presentation generation; a device without timestamp
    // queries, a zero period or a non-graphics queue leaves the CPU fallback in place.
    gpu_timestamps_usable = device->timestamp_capability().usable();
    pending_gpu_frame_time_ms.reset();
    rhi::SwapchainDesc swap_desc;
    swap_desc.extent = {metrics.physical_width, metrics.physical_height};
    swap_desc.image_count = 2;
    swap_desc.vsync = options.vsync;
    // A translucent window needs per-pixel alpha from the presentation surface (glass-v1.md section
    // 6). The platform already resolved whether the window system can present it; the swapchain is
    // the final gate. The request is a value: a surface without a premultiplied mode presents
    // opaque and the fallback below reports it once.
    const bool wants_transparency = metrics.transparent;
    swap_desc.per_pixel_alpha = wants_transparency;
    swap_desc.native_window = window->native_handle().window;
    swap_desc.native_display = window->native_handle().display;
    auto created = device->create_swapchain(swap_desc);
    if (!created && swap_desc.per_pixel_alpha) {
        // The surface refused a per-pixel-alpha swapchain (a driver or surface without the mode,
        // even though the window system reported it can show one). Retrying opaque keeps the window
        // usable instead of failing the host, and the fallback is reported once below.
        swap_desc.per_pixel_alpha = false;
        created = device->create_swapchain(swap_desc);
    }
    if (!created) {
        return false;
    }
    swapchain = std::move(*created);
    const bool alpha_presented = swapchain->alpha_mode() == rhi::SwapchainAlphaMode::PreMultiplied;
    translucent_presentation = wants_transparency && alpha_presented;
    if (wants_transparency && !alpha_presented && !alpha_fallback_reported) {
        alpha_fallback_reported = true;
        std::fprintf(stderr, "TESSERA_GLASS_WINDOW_UNSUPPORTED: the presentation surface offers no "
                             "per-pixel alpha; the window stays opaque\n");
    }
    // Fixed for this swapchain generation: whether a smaller render target (the adaptive render
    // scale) can be resampled into the presented images (docs/design/performance.md section 6.1).
    scaled_present_supported = swapchain->supports_scaled_present();
    for (std::size_t index = 0; index < scaled_targets.size(); ++index) {
        scaled_targets[index].reset();
        scaled_target_extents[index] = {};
    }

    pipeline = detail::create_pipeline(*device, swapchain->format());
    if (pipeline == nullptr) {
        return false;
    }
    detail::GlassPipelines glass = detail::create_glass_pipelines(*device, swapchain->format());
    if (glass.blur == nullptr || glass.composite == nullptr) {
        return false;
    }
    white = detail::create_white(*device);
    if (white == nullptr) {
        return false;
    }
    rendering::gpu::Batched2DConfig renderer_config{4096, true, false};
    renderer_config.frames_in_flight = static_cast<std::uint32_t>(detail::kFramesInFlight);
    // Glass bounds the blurred-backdrop pool (glass-v1 section 5); 0 disables the pool and a frame
    // then paints without the effect instead of growing without bound.
    renderer_config.max_offscreen_targets = options.runtime.glass_max_blur_targets;
    // The same limits the paint pass applies: the sigma clamp and the half-resolution threshold.
    renderer_config.glass.max_blur_radius = options.runtime.glass_max_blur_radius;
    renderer_config.glass.max_backdrop_area = options.runtime.glass_max_backdrop_area;
    glass_blur_pipeline = std::move(glass.blur);
    glass_composite_pipeline = std::move(glass.composite);
    renderer = std::make_unique<rendering::gpu::Batched2DRenderer>(
        *device, *pipeline, *white, renderer_config,
        rendering::gpu::GlassPipelines{glass_blur_pipeline.get(), glass_composite_pipeline.get()});
    // The text stack owns the glyph atlas textures, so it is recreated with the device generation
    // instead of reusing an atlas whose images died with the old device.
    text = std::make_unique<detail::TextStack>(*device, options.font_path);
    sink = std::make_unique<ui::paint::Batched2DSink>(*renderer, text->renderer.get(),
                                                      text->shaper.get());
    return true;
}

void WindowHost::Impl::release_presentation() {
    // Destruction order is the dependency order: the sink borrows the renderer and the text
    // renderer, and the renderer borrows the pipeline and the white texture. The adaptive-scale
    // target is an independent offscreen image, released before the swapchain it presents through.
    sink.reset();
    renderer.reset();
    text.reset();
    white.reset();
    for (std::size_t index = 0; index < scaled_targets.size(); ++index) {
        scaled_targets[index].reset();
        scaled_target_extents[index] = {};
    }
    scaled_present_supported = false;
    gpu_timestamps_usable = false;
    pending_gpu_frame_time_ms.reset();
    pipeline.reset();
    glass_blur_pipeline.reset();
    glass_composite_pipeline.reset();
    swapchain.reset();
    for (auto& commands : in_flight) {
        commands.reset();
    }
}

bool WindowHost::Impl::recreate_presentation() {
    release_presentation();
    try {
        if (create_presentation()) {
            return true;
        }
    } catch (const std::exception&) {
        // Backend factory calls throw only on contract violations (a lost/invalid device); a
        // recovery attempt reports a value instead of unwinding the driver (ADR-025 boundary).
    }
    release_presentation();
    return false;
}

bool WindowHost::Impl::replace_device() {
    // The one-shot in-place recover() of this loss is spent, so the backend device object itself is
    // rebuilt. The platform window owns the surface handle and outlives every device generation, so
    // the swapchain is recreated against the same native window afterwards; the input queue and the
    // accessibility root seam live on the window and stay attached.
    loop->set_device_recovery(nullptr);
    recovery.reset();
    release_in_flight(detail::kRecoveryDrainBudgetSeconds);
    release_presentation();
    // The replacement device is created for the same presentation family: the platform window and
    // its native surface handle outlive the lost device generation.
    device = detail::create_device(options.validation,
                                   detail::surface_platform_for(window->window_system()));
    if (device == nullptr) {
        return false;
    }
    recovery = std::make_unique<runtime::DeviceRecoveryController>(*device);
    loop->set_device_recovery(recovery.get());
    return true;
}

bool WindowHost::Impl::service_device_loss(const runtime::RuntimeTick& tick, RecoveryPolicy& policy,
                                           FrameFailureGuard& failures) {
    stats.device_state = WindowDeviceState::Lost;
    if (tick.device_failed) {
        // The backend's in-place attempt for this loss failed. A rebuilt device is the only
        // remaining move, and it is charged like any other attempt.
        const RecoveryAction action = policy.on_failed();
        stats.recovery_attempts = policy.attempts();
        if (action != RecoveryAction::Rebuild) {
            stats.stalled = true;
            return false;
        }
        if (!replace_device() || !recreate_presentation()) {
            // No device, or no presentation surface, for the fresh generation: nothing is left to
            // try in this run.
            stats.stalled = true;
            return false;
        }
        ++stats.recovered;
        stats.device_state = WindowDeviceState::Ready;
        failures.succeeded();
        // The damage/redraw state describes the lost generation, so the next frame is a full
        // repaint of the current window metrics.
        loop->mark_render_dirty();
        return true;
    }

    switch (policy.on_blocked()) {
    case RecoveryAction::Release:
        // Destroy the lost-generation RHI objects once: the controller's next probe can only
        // succeed after they are gone (`ResourcesOutstanding` otherwise).
        release_in_flight(detail::kRecoveryDrainBudgetSeconds);
        release_presentation();
        break;
    case RecoveryAction::GiveUp:
        stats.stalled = true;
        return false;
    default:
        break;
    }
    stats.recovery_attempts = policy.attempts();
    ++stats.idle_waits;
    // A controller that never reports a recovery must still terminate the run, and the wait between
    // attempts is bounded (a lost device has no pacer deadline to park on).
    if (failures.failed()) {
        stats.stalled = true;
        return false;
    }
    idle(kFrameRetryWaitSeconds);
    return true;
}

bool WindowHost::Impl::service_recovery(RecoveryPolicy& policy, FrameFailureGuard& failures) {
    policy.on_recovered();
    stats.recovery_attempts = policy.attempts();
    // The window outlives the device, so its input queue and accessibility root seam stayed
    // attached; re-asserting the queue costs nothing and keeps the pump the single owner.
    window->set_input_queue(&pump.queue());
    if (!recreate_presentation()) {
        // The device is healthy but the presentation stack did not come back (a swapchain the
        // surface refuses, a lost font, an out-of-memory creation). One budgeted rebuild on a fresh
        // device is the remaining move.
        if (policy.on_failed() != RecoveryAction::Rebuild || !replace_device() ||
            !recreate_presentation()) {
            stats.stalled = true;
            stats.device_state = WindowDeviceState::Lost;
            return false;
        }
        stats.recovery_attempts = policy.attempts();
    }
    ++stats.recovered;
    stats.device_state = WindowDeviceState::Ready;
    failures.succeeded();
    // Damage state referred to the lost swapchain generation: repaint the whole frame.
    loop->mark_render_dirty();
    return true;
}

} // namespace tessera::app
