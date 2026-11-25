// SPDX-License-Identifier: MIT
//
// The window driver's frame path (issue #228): one frame attempt - acquire, paint, present - and
// the swapchain frame-pacing sample that follows it. Split out of `window.cpp` when the
// multi-window refactor pushed that file past its size budget; the boundary is the one the code
// already had, since `render_and_present` is the only function that touches the acquired image.
//
// The shared caches this path draws through (the renderer, the text stack, the sink, the pipelines)
// belong to the `WindowDevice` group, not to this window: they are reached through the
// `renderer()`, `text()`, `sink()` and `present_pipeline()` accessors so a device generation change
// cannot leave a stale pointer here. The swapchain, the scaled targets and the in-flight command
// lists are this window's own.

#include "tessera/app/frame_guard.hpp"
#include "tessera/app/recovery_policy.hpp"
#include "tessera/rendering/rhi/swapchain.hpp"
#include "tessera/runtime/frame_time_source.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scale/render_target_extent.hpp"
#include "window_impl.hpp"
#include "window_vector.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>

namespace tessera::app {

namespace hal = tessera::hal;
namespace rhi = tessera::rendering::rhi;

// One rendered frame: reuse a free slot, acquire, paint the whole scene, submit async and present.
// `full_repaint` is the plan's verdict from the host frame; it is recorded so a caller can prove a
// resume repainted the whole target.
WindowHost::Impl::FrameResult WindowHost::Impl::render_and_present(bool full_repaint) {
    if (device == nullptr || swapchain == nullptr) {
        return FrameResult::Skipped;
    }
    sample_frame_sync();
    // A slot whose GPU work is still running is skipped, never waited on: the UI thread must not
    // block on the GPU (ADR-021). The frame is retried on the next tick. This is backpressure, not
    // a failure: under load a busy slot can stay busy for longer than a frame, so it reports Busy
    // and leaves the failure budget alone.
    if (in_flight[slot] && !in_flight[slot]->is_complete()) {
        // A lost device never completes it either; report the loss instead of skipping forever.
        return device->status() == rhi::DeviceStatus::Lost ? FrameResult::DeviceLost
                                                           : FrameResult::Busy;
    }
    // The slot's GPU work has completed (a non-blocking is_complete() poll above), so its recorded
    // timestamp pair can be read without waiting (ADR-021). The interval is consumed at this
    // frame's observation and is at most two frames old: a value is only readable after its
    // submission completed, which is what keeps the read a host query instead of a wait.
    if (in_flight[slot]) {
        double gpu_frame_time_ms = 0.0;
        if (in_flight[slot]->gpu_frame_time_ms(gpu_frame_time_ms)) {
            frame_time.publish(gpu_frame_time_ms);
        }
        in_flight[slot].reset();
    }

    // Adaptive render scale (docs/design/performance.md section 6, step 2): the scene may render
    // into a target of the presented pixel grid times the controller's renderScale, and the frame
    // is presented through it. The multiplier is read from the decision in force, so a change lands
    // at this frame boundary. At scale 1.0 the extents are equal and the frame takes the direct
    // path, byte for byte the full-quality frame. The offscreen choice itself (adaptive scale,
    // glass, 4x MSAA) is prepare_paint_target's, resolved before the image is acquired: a 4x
    // presentation whose offscreen target cannot be allocated skips the attempt without leaving an
    // acquired image unpresented.
    const rhi::Extent2D presented = swapchain->extent();
    const ui::scale::AdaptiveTargetExtent adaptive =
        ui::scale::adaptive_target_extent(presented, host->adaptive_quality().render_scale);
    std::optional<PaintTarget> paint = prepare_paint_target(slot, presented, adaptive);
    if (!paint.has_value()) {
        return FrameResult::Skipped;
    }

    std::optional<std::uint32_t> image;
    for (int attempt = 0; attempt < 4 && !image; ++attempt) {
        const auto acquired = swapchain->acquire_next_image(100);
        if (acquired) {
            image = *acquired;
            break;
        }
        if (acquired.error() == rhi::SwapchainError::OutOfDate) {
            // A same-size resize is a documented no-op, so recreate at the window's real size.
            recreate_swapchain();
            continue;
        }
        if (acquired.error() == rhi::SwapchainError::DeviceLost) {
            return FrameResult::DeviceLost;
        }
        break;
    }
    if (!image) {
        return FrameResult::Skipped;
    }
    rhi::RenderTarget* target = swapchain->image_target(*image);
    if (target == nullptr) {
        return FrameResult::Skipped;
    }
    if (!paint->offscreen) {
        paint->target = target;
    }
    const bool msaa = paint->msaa;
    rhi::RenderTarget* paint_target = paint->target;
    const rhi::RenderTargetDesc& desc = paint->desc;
    const bool offscreen = paint->offscreen;
    const rhi::Color clear_color = detail::frame_clear_color(translucent_presentation);
    auto commands = device->create_command_list();
    // Arm the GPU timestamp pair before the first command is recorded. A false return (the probe
    // says timestamps are unusable, or the backend does not implement them) costs nothing and the
    // frame keeps the CPU signal.
    (void)commands->begin_gpu_timestamps(static_cast<std::uint32_t>(slot));
    renderer()->begin(*commands, *paint_target, desc);
    // CSS -> target: the device scale composed with the adaptive target/presented factor.
    // Positions, clip rects and the T1 CSS-sized text quads all take it; at 1.0 it is not applied
    // at all.
    const ui::scale::ContentScale transform = apply_frame_transform(*renderer(), adaptive);
    stats.content_scale_x = transform.x;
    stats.content_scale_y = transform.y;
    sink()->begin_text_frame();
    // The vector raster extent follows this window's scale, which changes with its monitor.
    sink()->set_vector_device_scale(device_scale);
    // A presented frame is a full redraw (swapchain images are not preserved); glass limits are the
    // style sync's.
    ui::paint::PaintContext context;
    context.glass.max_blur_radius = options.runtime.glass_max_blur_radius;
    context.glass.max_backdrop_area = options.runtime.glass_max_backdrop_area;
    context.text_metrics_source = text()->paint_metrics.get();
    context.device_scale = device_scale;
    context.lcd_text =
        lcd_pipeline() != nullptr && aa_policy.text_mode == ui::scale::TextAaMode::LcdWhenEligible;
    host->paint_scene(*sink(), context);
    // The last SVG acquire of this frame has happened: submit the queued GPU rasters now, so they
    // are ahead of the draw list that samples their textures.
    if (detail::VectorStack* const vectors = vector(); vectors != nullptr) {
        vectors->flush();
    }
    renderer()->end();
    if (msaa) {
        // A 4x target's multisample image is not a copy/blit source, so the frame is presented
        // through its resolved image instead (RHI 1.3 sampled target): one full-target quad on the
        // single-sample, no-blend present pipeline. This is the GPU-only present path - the POC's
        // CPU readback bridge is not used. The same submission still carries the acquire wait and
        // the present semaphore, exactly like a render pass into the acquired image would.
        renderer()->present_target(*commands, *target,
                                   detail::target_desc(presented, swapchain->format(), clear_color),
                                   *paint_target, *present_pipeline());
    } else if (offscreen) {
        // The same submission records the resample into the acquired image, so it carries the
        // acquire wait and the present semaphore exactly like a render pass into that image would.
        commands->blit_target(*scaled_targets.get(slot), *target, rhi::Filter::Linear);
    }
    stats.render_scale = host->adaptive_quality().render_scale;
    stats.present_extent = presented;
    stats.target_extent = paint_target->extent();
    stats.device_scale = device_scale;
    stats.aa_policy = aa_policy;
    stats.sample_count = presentation_sample_count_;
    if (paint->scaled) {
        ++stats.scaled_frames;
    }
    commands->submit_async();
    in_flight[slot] = std::move(commands);
    slot = (slot + 1) % detail::kFramesInFlight;
    ++stats.frames;
    if (detail::VectorStack* const vectors = vector(); vectors != nullptr) {
        // `slot` now names the older frame; a texture evicted during this frame is only destroyed
        // once that frame finished, because it may still sample it.
        vectors->end_frame(!in_flight[slot] || in_flight[slot]->is_complete());
        const auto cache = vectors->stats();
        stats.vector_gpu_rasterizations = cache.gpu_rasterizations;
        stats.vector_gpu_fallbacks = cache.gpu_fallbacks;
    }
    stats.vector_draws = sink()->vector_stats().draws;

    const auto present = swapchain->present(*image);
    if (present || present.error() == rhi::SwapchainError::Suboptimal) {
        ++stats.presented;
        if (full_repaint) {
            ++stats.full_repaints;
        }
    } else if (present.error() == rhi::SwapchainError::OutOfDate) {
        recreate_swapchain();
    } else if (present.error() == rhi::SwapchainError::DeviceLost) {
        return FrameResult::DeviceLost;
    }
    // Sample after the submit so the reported timeline value includes this frame's signal; the
    // GPU may still be running it, which is exactly what frames_in_flight reports.
    sample_frame_sync();
    return FrameResult::Presented;
}

void WindowHost::Impl::sample_frame_sync() {
    if (swapchain == nullptr) {
        return;
    }
    const rhi::SwapchainFrameSync sync = swapchain->frame_sync();
    stats.timeline_value = sync.timeline_value;
    stats.frames_in_flight = sync.frames_in_flight;
    stats.slots_skipped = sync.slots_skipped;
}

runtime::FrameTimeSample WindowHost::Impl::observe_frame_time(double cpu_frame_time_ms) {
    // The selection rule and the capability bookkeeping live in detail::FrameTimeProbe; the host
    // records the verdict in stats and hands the raw numbers to the adaptive controller.
    const detail::FrameTimeObservation observation = frame_time.observe(cpu_frame_time_ms);
    stats.frame_time_source = observation.sample.source;
    if (observation.sample.source == runtime::FrameTimeSource::Gpu) {
        ++stats.gpu_frame_time_samples;
    } else if (observation.counted_fallback()) {
        ++stats.gpu_frame_time_fallbacks;
    }
    host->observe_frame_time_sample(cpu_frame_time_ms, observation.gpu_frame_time_ms);
    return observation.sample;
}

} // namespace tessera::app
