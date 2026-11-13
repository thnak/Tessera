// SPDX-License-Identifier: MIT

#include "tessera/app/frame_guard.hpp"
#include "tessera/app/idle_policy.hpp"
#include "tessera/app/recovery_policy.hpp"
#include "tessera/app/system_motion.hpp"
#include "tessera/hal/refresh_rate.hpp"
#include "tessera/hal/timer/steady_time.hpp"
#include "tessera/hal/window_wait.hpp"
#include "tessera/rendering/rhi/swapchain.hpp"
#include "tessera/runtime/frame_time_source.hpp"
#include "tessera/ui/input.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scale/render_target_extent.hpp"
#include "window_backdrop_request.hpp"
#include "window_impl.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace tessera::app {
namespace {

namespace hal = tessera::hal;
namespace rhi = tessera::rendering::rhi;

} // namespace

WindowHost::Impl::Impl(ui::Node& root, ui::style::CompiledStyleSheet sheet,
                       WindowOptions window_options, ui::UpdateScheduler* update_scheduler)
    : root(root),
      options(std::move(window_options)),
      scheduler(update_scheduler),
      pacer(runtime::FramePacerConfig{options.max_fps, options.refresh_hz, false}) {
    // No field owns focus until the app says so. An inactive blink is not ticked, so a window that
    // never gains an editable field never requests a caret frame (ADR-019: static UI idles).
    (void)caret.set_active(false, caret_now_seconds());
    applied_present_cap_hz = options.max_fps;
    // The runtime config's lifecycle keys are mapped here, once: the pause policy owns occlusion
    // and minimization, and the pipeline config owns paint interpolation.
    pause_policy = window_pause_policy(options.runtime);
    const hal::WindowBackdrop backdrop_request = window_backdrop_request(root, options.backdrop);
    hal::WindowDesc desc;
    desc.title = options.title;
    desc.width = options.width;
    desc.height = options.height;
    desc.visible = options.visible;
    desc.backdrop = backdrop_request;
    desc.transparent = options.transparent;
    desc.win10_acrylic = options.win10_acrylic;
    window = hal::create_window(desc);
    if (window == nullptr) {
        return;
    }
    // Resolve the display refresh now that the platform window exists: the platform query fills in
    // WindowOptions::refresh_hz when the caller left it at 0 (unknown), and an explicit non-zero
    // value still wins. No query runs on the explicit path.
    //
    // The pacer is deliberately not reconfigured here. Its uncapped interval would become one
    // display refresh, which would gate the deterministic single-iteration seam (pump_once) on the
    // wall clock: an automation caller that changes the scene and pumps once must still present,
    // and a settled default window keeps the documented present-when-ready behaviour. When the
    // adaptive present cap actually applies a divisor, apply_present_cap() hands the pacer both
    // that cap and this refresh through its normal rate-change path, so the divisor grid engages
    // exactly when there is a cap to place on it. An explicit refresh keeps its existing pacing.
    const double reported_refresh_hz =
        options.refresh_hz == 0.0 ? window->display_refresh_hz() : hal::kUnknownRefreshHz;
    options.refresh_hz = hal::resolve_display_refresh_hz(options.refresh_hz, reported_refresh_hz);
    device = detail::create_device(options.validation);
    if (device == nullptr) {
        window.reset();
        return;
    }
    if (!create_presentation()) {
        release_presentation();
        device.reset();
        window.reset();
        return;
    }
    recovery = std::make_unique<runtime::DeviceRecoveryController>(*device);
    loop->set_device_recovery(recovery.get());

    const hal::WindowMetrics metrics = window->metrics();
    // The platform resolved the requested OS backdrop at creation (glass-v1.md section 6). The
    // unsupported path is one warning and the opaque fallback; it never delays, retries or fails
    // startup for a cosmetic material. The stylesheet's `@media (window-backdrop: none)` rule is
    // what supplies an opaque background.
    if (metrics.backdrop_fallback) {
        const std::string_view requested = hal::window_backdrop_name(backdrop_request);
        std::fprintf(stderr,
                     "TESSERA_GLASS_WINDOW_UNSUPPORTED: window backdrop `%.*s` is unavailable on "
                     "this platform; the window stays opaque\n",
                     static_cast<int>(requested.size()), requested.data());
    }
    HostConfig config;
    config.width = static_cast<float>(metrics.logical_width);
    config.height = static_cast<float>(metrics.logical_height);
    config.dpi_scale = metrics.dpi_scale;
    config.text_measurer = options.text_measurer;
    config.reduced_motion = system_prefers_reduced_motion();
    config.pipeline = window_pipeline_config(options.runtime);
    config.adaptive_quality = window_adaptive_quality_policy(options.runtime, options.refresh_hz);
    motion = MotionPreference(config.reduced_motion);
    host = std::make_unique<AppHost>(root, std::move(sheet), *loop, config);
    if (scheduler != nullptr) {
        host->set_update_scheduler(scheduler);
    }
    window->set_input_queue(&pump.queue());
    // A platform can create the window already occluded or minimized; start paused in that case so
    // the first iteration does not acquire and present a frame the user cannot see. The policy can
    // disable that pause (`pauseWhenOccluded=false`), in which case the window keeps rendering.
    metrics_paused_ = pause_policy.pauses(metrics.occluded, metrics.minimized);
    paused_ = metrics_paused_;
    last_tick_seconds_ = hal::steady_now_seconds();
    available = true;
}

WindowHost::Impl::~Impl() {
    if (window != nullptr) {
        window->set_input_queue(nullptr);
    }
    loop->set_device_recovery(nullptr);
    // No device-wide wait_idle(): drain only the lists this loop submitted, with a hard budget, and
    // let the device's own destruction be the deferred full drain.
    release_in_flight(detail::kDrainBudgetSeconds);
}

// Parks on the bounded platform wait for at most `seconds` (already clamped by the caller). Returns
// as soon as the window has an event and never sleeps past the deadline: the HAL wait slices at
// 50 ms and re-checks. A zero budget still pumps the message queue without sleeping so a close
// queued during a long idle is never missed.
void WindowHost::Impl::idle(double seconds) {
    if (!(seconds > 0.0) || !std::isfinite(seconds)) {
        (void)window->wait_for_events(0.0);
        return;
    }
    (void)hal::wait_for_events_until(*window, hal::steady_now_seconds() + seconds);
}

void WindowHost::Impl::release_in_flight(double budget_seconds) {
    if (device != nullptr && device->status() == rhi::DeviceStatus::Lost) {
        // A lost device never signals its fences, so polling would spin the UI thread for the whole
        // budget. The generation is closed out instead: the RHI reclaims the command resources with
        // the device, and every list from before the loss is unusable anyway.
        for (auto& commands : in_flight) {
            commands.reset();
        }
        return;
    }
    const double budget = budget_seconds > 0.0 ? budget_seconds : 0.0;
    const double deadline = hal::steady_now_seconds() + budget;
    for (auto& commands : in_flight) {
        while (commands != nullptr && !commands->is_complete() &&
               hal::steady_now_seconds() < deadline) {
            (void)commands->wait(detail::kPollSliceMs);
        }
        commands.reset();
    }
}

// Recreates the swapchain at the window's current physical size. OutOfDate means the surface
// changed size; resize(extent()) would be the documented no-op and never clear the error.
void WindowHost::Impl::recreate_swapchain() {
    if (swapchain == nullptr) {
        return;
    }
    const hal::WindowMetrics current = window->metrics();
    swapchain->resize({current.physical_width, current.physical_height});
}

// The offscreen target a downgraded frame renders into. It is created on the first frame of a new
// decided extent and reused while that extent lasts, so a held downgrade costs one image per frame
// slot and no per-frame allocation; an upgrade to full quality stops using it (and the next
// downgrade at the same extent reuses it). A backend refusal is a value, not a failure: the frame
// presents at full quality instead (ADR-025 boundary, and a lost frame would be worse than a sharp
// one).
bool WindowHost::Impl::ensure_scaled_target(std::size_t frame_slot, rhi::Extent2D extent) {
    if (swapchain == nullptr || frame_slot >= scaled_targets.size() || extent.width == 0 ||
        extent.height == 0) {
        return false;
    }
    const rhi::TextureFormat format = swapchain->format();
    rhi::RenderTarget* existing = scaled_targets[frame_slot].get();
    if (existing != nullptr && scaled_target_extents[frame_slot].width == extent.width &&
        scaled_target_extents[frame_slot].height == extent.height && existing->format() == format) {
        return true;
    }
    try {
        scaled_targets[frame_slot] = device->create_render_target(detail::target_desc(
            extent, format,
            translucent_presentation ? rhi::rgba8(0, 0, 0, 0) : rhi::rgba8(11, 18, 32)));
        scaled_target_extents[frame_slot] = extent;
        return scaled_targets[frame_slot] != nullptr;
    } catch (const std::exception&) {
        scaled_targets[frame_slot].reset();
        scaled_target_extents[frame_slot] = {};
        ++stats.scale_fallbacks;
        return false;
    }
}

// One rendered frame: reuse a free slot, acquire, paint the whole scene, submit async and present.
// `full_repaint` is the plan's verdict from the host frame; it is recorded so a caller can prove a
// resume repainted the whole target.
WindowHost::Impl::FrameResult WindowHost::Impl::render_and_present(bool full_repaint) {
    if (device == nullptr || swapchain == nullptr) {
        return FrameResult::Skipped;
    }
    sample_frame_sync();
    // A slot whose GPU work is still running is skipped, never waited on: the UI thread must not
    // block on the GPU (ADR-021). The frame is retried on the next tick.
    if (in_flight[slot] && !in_flight[slot]->is_complete()) {
        // A lost device never completes it either; report the loss instead of skipping forever.
        return device->status() == rhi::DeviceStatus::Lost ? FrameResult::DeviceLost
                                                           : FrameResult::Skipped;
    }
    // The slot's GPU work has completed (a non-blocking is_complete() poll above), so its recorded
    // timestamp pair can be read without waiting (ADR-021). The interval is consumed at this
    // frame's observation and is at most two frames old: a value is only readable after its
    // submission completed, which is what keeps the read a host query instead of a wait.
    if (in_flight[slot]) {
        double gpu_frame_time_ms = 0.0;
        if (in_flight[slot]->gpu_frame_time_ms(gpu_frame_time_ms)) {
            pending_gpu_frame_time_ms = gpu_frame_time_ms;
        }
        in_flight[slot].reset();
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

    // Adaptive render scale (docs/design/performance.md section 6, step 2): the scene renders into
    // a target of the presented pixel grid times the controller's renderScale, and the present
    // resamples it up. The multiplier is read from the decision in force, so a change lands at this
    // frame boundary - never mid-frame and never per pixel. At scale 1.0 the extents are equal and
    // the frame takes the direct path below, byte for byte today's full-quality frame.
    const rhi::Extent2D presented = swapchain->extent();
    const ui::scale::AdaptiveTargetExtent adaptive =
        ui::scale::adaptive_target_extent(presented, host->adaptive_quality().render_scale);
    // A backend whose swapchain images cannot be resampled keeps the direct path instead of
    // dropping frames; the capability is fixed for this swapchain generation.
    const bool scaled =
        adaptive.scaled && scaled_present_supported && ensure_scaled_target(slot, adaptive.extent);
    const rhi::Color clear_color =
        translucent_presentation ? rhi::rgba8(0, 0, 0, 0) : rhi::rgba8(11, 18, 32);
    rhi::RenderTarget* paint_target = target;
    rhi::RenderTargetDesc desc = detail::target_desc(presented, swapchain->format(), clear_color);
    if (scaled) {
        paint_target = scaled_targets[slot].get();
        desc = detail::target_desc(paint_target->extent(), swapchain->format(), clear_color);
    }
    auto commands = device->create_command_list();
    // Arm the GPU timestamp pair before the first command is recorded. A false return (the probe
    // says timestamps are unusable, or the backend does not implement them) costs nothing and the
    // frame keeps the CPU signal.
    (void)commands->begin_gpu_timestamps(static_cast<std::uint32_t>(slot));
    renderer->begin(*commands, *paint_target, desc);
    if (scaled) {
        // Presented pixels -> target pixels. Positions and clip rects scale; layout, glyph raster
        // and the atlas do not (the atlas is keyed by font pixel size, never by this factor).
        renderer->set_content_scale(adaptive.content_scale_x, adaptive.content_scale_y);
    }
    sink->begin_text_frame();
    // Swapchain images are not preserved across presents, so a presented frame is a full redraw;
    // the damage plan only decided whether this frame happens at all.
    ui::paint::paint_scene(root, host->styles(), *sink);
    renderer->end();
    if (scaled) {
        // The same submission records the resample into the acquired image, so it carries the
        // acquire wait and the present semaphore exactly like a render pass into that image would.
        commands->blit_target(*scaled_targets[slot], *target, rhi::Filter::Linear);
    }
    stats.render_scale = host->adaptive_quality().render_scale;
    stats.present_extent = presented;
    stats.target_extent = paint_target->extent();
    if (scaled) {
        ++stats.scaled_frames;
    }
    commands->submit_async();
    in_flight[slot] = std::move(commands);
    slot = (slot + 1) % detail::kFramesInFlight;
    ++stats.frames;

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

std::optional<double> WindowHost::Impl::take_gpu_frame_time_ms() {
    if (!pending_gpu_frame_time_ms) {
        return std::nullopt;
    }
    const double milliseconds = *pending_gpu_frame_time_ms;
    pending_gpu_frame_time_ms.reset();
    return milliseconds;
}

runtime::FrameTimeSample WindowHost::Impl::observe_frame_time(double cpu_frame_time_ms) {
    // The selection rule is pure (tessera/runtime/frame_time_source.hpp); the capability decision
    // was made once by the RHI probe. A capability that is usable but has no fresh interval (the
    // first frames, a skipped read, an unusable value) falls back to the CPU time and is counted,
    // so a reader can tell a real GPU number from a fallback.
    const std::optional<double> gpu_frame_time_ms = take_gpu_frame_time_ms();
    const runtime::FrameTimeSample sample =
        runtime::select_frame_time(cpu_frame_time_ms, gpu_frame_time_ms);
    stats.frame_time_source = sample.source;
    if (sample.source == runtime::FrameTimeSource::Gpu) {
        ++stats.gpu_frame_time_samples;
    } else if (gpu_timestamps_usable) {
        ++stats.gpu_frame_time_fallbacks;
    }
    host->observe_frame_time_sample(cpu_frame_time_ms, gpu_frame_time_ms);
    return sample;
}

// Applies one metrics snapshot. Occlusion has no Win32/X11 event yet, so the snapshot's
// `occluded`/`minimized` flags are the channel a platform (and a test) reports the lifecycle
// through: they own the metrics half of the pause state.
void WindowHost::Impl::apply_metrics(const hal::WindowMetrics& metrics) {
    if (!available) {
        return;
    }
    metrics_paused_ = pause_policy.pauses(metrics.occluded, metrics.minimized);
    update_pause_state();
    // A metrics snapshot does not change the presentation generation, so `translucent_presentation`
    // keeps following the current swapchain's composite mode; a later recreation (a resize
    // OutOfDate, a device recovery) re-resolves it from the platform's fresh metrics.
    // An occluded/minimized window can report a zero client area (a minimized Win32 window does);
    // resizing the target or re-laying out at 0x0 is skipped whenever the metrics say invisible,
    // even when the pause policy keeps rendering (`pauseWhenOccluded=false`). The scene tree stays
    // intact (docs/design/window-resize.md section 2 rule 6); the restore's metrics apply the real
    // size.
    if (!metrics.occluded && !metrics.minimized) {
        if (swapchain != nullptr) {
            swapchain->resize({metrics.physical_width, metrics.physical_height});
        }
        host->resize(static_cast<float>(metrics.logical_width),
                     static_cast<float>(metrics.logical_height), metrics.dpi_scale);
    }
    loop->mark_render_dirty();
}

WindowFrameStats WindowHost::Impl::run(std::uint32_t max_frames) {
    stats = {};
    if (!available) {
        return stats;
    }
    stats.available = true;
    stats.device_state = WindowDeviceState::Ready;
    // The platform's resolved backdrop is creation-time state, not a per-frame query.
    const hal::WindowMetrics window_metrics = window->metrics();
    stats.backdrop = window_metrics.backdrop;
    stats.transparent = window_metrics.transparent;
    const bool force_frames = max_frames > 0;
    // A previous run detached the queue so a platform event could not outlive its pump, so a second
    // run() reattaches it on entry instead of leaving the window deaf forever.
    window->set_input_queue(&pump.queue());
    pacer.reset();
    // The attempt budget bounds one loop invocation: a caller that retries run() after a give-up
    // starts a fresh bounded recovery sequence (app/recovery_policy.hpp).
    RecoveryPolicy recovery_policy;

    bool closed = false;
    last_tick_seconds_ = hal::steady_now_seconds();
    // A frame that keeps failing (a device that never recovers, a slot that never completes, a
    // swapchain that never yields an image) stops the run instead of spinning (ADR-021).
    FrameFailureGuard failures;
    std::array<hal::WindowEvent, detail::kMaxWindowEvents> events{};
    // Startup is a frame cause: the scene has no dirty flags until the first layout.
    loop->mark_render_dirty();

    while (!closed) {
        const std::size_t event_count = window->poll_events(events);
        for (std::size_t index = 0; index < event_count; ++index) {
            closed = handle_window_event(events[index]) || closed;
        }
        if (closed) {
            break;
        }

        pump_input();
        if (force_frames && stats.frames < max_frames) {
            loop->mark_render_dirty();
        }

        // A paused window (occluded, minimized or hidden by the platform) produces no frame: no
        // acquire, submit or present and no style/layout/paint or animation tick. The loop keeps
        // pumping events and input and parks on the same bounded platform wait, so a close, a
        // resize/metrics change or a restore wakes it. The pause wins over the forced-frame path:
        // run(max_frames) waits for the window to become visible again or to close.
        if (paused()) {
            ++stats.paused_iterations;
            // Drop the paused wall time so the first frame after resume gets a normal dt instead
            // of the whole pause duration.
            last_tick_seconds_ = hal::steady_now_seconds();
            idle(idle_wait_seconds(pacer.ready(last_tick_seconds_)));
            continue;
        }

        // After the pause guard and before the frame decision: a paused window never ticks the
        // caret (pause wins over the blink), and a flip requests exactly one frame for this
        // iteration.
        tick_caret();

        const runtime::RuntimeTick tick = loop->tick();
        if (tick.device_blocked || tick.device_failed) {
            // No presents while the device is lost: release once, let the runtime's bounded attempt
            // run, and park on the platform wait between attempts (never a spin).
            if (!service_device_loss(tick, recovery_policy, failures)) {
                break;
            }
            continue;
        }
        if (tick.device_recovered) {
            if (!service_recovery(recovery_policy, failures)) {
                break;
            }
            continue;
        }
        if (!tick.should_render) {
            // Settled scene: do not begin a host frame at all, so no style, layout or paint work
            // runs. Park until the next scheduled present or a wake source.
            ++stats.idle_waits;
            idle(idle_wait_seconds(pacer.ready(hal::steady_now_seconds())));
            continue;
        }

        // A frame the runtime wants still respects the present-rate cap: idle to the pacer deadline
        // instead of rendering early and sleeping a fixed amount afterwards.
        const double now = hal::steady_now_seconds();
        const runtime::PaceDecision pace = pacer.ready(now);
        if (!pace.render_now) {
            ++stats.idle_waits;
            idle(pacer_wait_seconds(pace));
            continue;
        }

        const double dt = now - last_tick_seconds_;
        last_tick_seconds_ = now;
        const HostFrame frame = host->begin_frame(dt, tick.alpha);
        const bool has_damage = !frame.render.redraw_plan.scissors.empty();
        if (!should_present(frame.frame_due, has_damage, force_frames)) {
            // A wake that changed nothing: clear the dirt the tick collected so it cannot stay
            // latched into a phantom render request, then idle without painting. The forced CI path
            // is the only caller allowed to paint an unchanged scene.
            host->end_frame();
            loop->clear_render_dirty();
            ++stats.idle_waits;
            idle(idle_wait_seconds(pacer.ready(hal::steady_now_seconds())));
            continue;
        }

        const FrameResult outcome = render_and_present(frame.render.full_redraw);
        if (outcome != FrameResult::Presented) {
            ++stats.idle_waits;
            // The backend recorded a loss on this frame; the next tick's controller reports it
            // blocked and service_device_loss() takes over. Counting it here keeps a runtime
            // without recovery support bounded by the same guard.
            if (failures.failed()) {
                stats.stalled = true;
                if (outcome == FrameResult::DeviceLost) {
                    stats.device_state = WindowDeviceState::Lost;
                }
                break;
            }
            // begin_frame() already consumed the latched full repaint and the damage plan, so the
            // retry would find nothing to draw and the window would keep its stale image. A skipped
            // frame (a busy slot, a failed acquire) asks for the whole target again.
            if (outcome == FrameResult::Skipped) {
                host->request_full_repaint();
            }
            // A busy slot may free on any GPU completion; retry on the bounded backoff.
            idle(kFrameRetryWaitSeconds);
            continue;
        }
        failures.succeeded();
        pacer.present(hal::steady_now_seconds());
        // Adaptive quality (docs/design/performance.md section 6): the one frame observation, fed
        // from the GPU interval of the most recent completed submission when the device's
        // timestamp probe reported it usable, else from this frame's CPU cost through present. A
        // skipped attempt returned above and is not an observation.
        (void)observe_frame_time((hal::steady_now_seconds() - now) * 1000.0);
        apply_present_cap(hal::steady_now_seconds());
        host->end_frame();
        loop->clear_render_dirty();
        if (force_frames && stats.frames >= max_frames) {
            break;
        }
        // Park until the next present is due (a zero budget only pumps the event queue when the
        // pacer is uncapped, so a continuous loop never spins on a timer).
        idle(pacer_wait_seconds(pacer.ready(hal::steady_now_seconds())));
    }

    window->set_input_queue(nullptr);
    release_in_flight(detail::kDrainBudgetSeconds);
    return stats;
}

WindowHost::WindowHost(ui::Node& root, ui::style::CompiledStyleSheet sheet, WindowOptions options,
                       ui::UpdateScheduler* scheduler)
    : impl_(nullptr) {
    // Validate before the pacer is constructed: a negative or non-finite cap would throw from a
    // member initializer where the message is opaque.
    if (!std::isfinite(options.max_fps) || options.max_fps < 0.0) {
        throw std::invalid_argument("tessera app window: max_fps must be finite and non-negative");
    }
    if (!std::isfinite(options.refresh_hz) || options.refresh_hz < 0.0) {
        throw std::invalid_argument("tessera app window: refresh_hz must be finite and >= 0");
    }
    impl_ = std::make_unique<Impl>(root, std::move(sheet), std::move(options), scheduler);
}

WindowHost::~WindowHost() = default;

bool WindowHost::available() const {
    return impl_->available;
}

WindowFrameStats WindowHost::run(std::uint32_t max_frames) {
    return impl_->run(max_frames);
}

bool WindowHost::pump_once() {
    return impl_->pump_once();
}

bool WindowHost::post_input(const ui::InputEvent& event) {
    if (!impl_->available) {
        return false;
    }
    return impl_->pump.queue().push(event);
}

void WindowHost::apply_metrics(const hal::WindowMetrics& metrics) {
    impl_->apply_metrics(metrics);
}

void WindowHost::set_caret_active(bool active) {
    impl_->set_caret_active(active);
}

bool WindowHost::caret_visible() const {
    return impl_->caret.caret_visible();
}

const WindowFrameStats& WindowHost::stats() const {
    return impl_->stats;
}

AppHost& WindowHost::host() {
    if (impl_->host == nullptr) {
        throw std::logic_error("tessera app window: host() requires an available window");
    }
    return *impl_->host;
}

#if defined(TESSERA_VULKAN_TEST_HOOKS)
rendering::rhi::Device& WindowHost::device_for_testing() {
    return *impl_->device;
}
#endif

} // namespace tessera::app
