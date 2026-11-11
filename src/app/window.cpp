// SPDX-License-Identifier: MIT

#include "tessera/app/frame_guard.hpp"
#include "tessera/app/idle_policy.hpp"
#include "tessera/app/recovery_policy.hpp"
#include "tessera/hal/timer/steady_time.hpp"
#include "tessera/hal/window_wait.hpp"
#include "tessera/rendering/rhi/swapchain.hpp"
#include "tessera/ui/input.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "window_impl.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>

namespace tessera::app {
namespace {

namespace hal = tessera::hal;
namespace rhi = tessera::rendering::rhi;

bool subtree_dirty(const ui::Node& root) {
    return root.subtree_dirty() != ui::Dirty::Clean || root.dirty() != ui::Dirty::Clean;
}

} // namespace

WindowHost::Impl::Impl(ui::Node& root, ui::style::CompiledStyleSheet sheet,
                       WindowOptions window_options, ui::UpdateScheduler* update_scheduler)
    : root(root),
      options(std::move(window_options)),
      scheduler(update_scheduler),
      pacer(runtime::FramePacerConfig{options.max_fps}) {
    hal::WindowDesc desc;
    desc.title = options.title;
    desc.width = options.width;
    desc.height = options.height;
    desc.visible = options.visible;
    window = hal::create_window(desc);
    if (window == nullptr) {
        return;
    }
    device = detail::create_device();
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
    HostConfig config;
    config.width = static_cast<float>(metrics.logical_width);
    config.height = static_cast<float>(metrics.logical_height);
    config.dpi_scale = metrics.dpi_scale;
    config.text_measurer = options.text_measurer;
    host = std::make_unique<AppHost>(root, std::move(sheet), *loop, config);
    if (scheduler != nullptr) {
        host->set_update_scheduler(scheduler);
    }
    window->set_input_queue(&pump.queue());
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

// One rendered frame: reuse a free slot, acquire, paint the whole scene, submit async and present.
WindowHost::Impl::FrameResult WindowHost::Impl::render_and_present() {
    if (device == nullptr || swapchain == nullptr) {
        return FrameResult::Skipped;
    }
    // A slot whose GPU work is still running is skipped, never waited on: the UI thread must not
    // block on the GPU (ADR-021). The frame is retried on the next tick.
    if (in_flight[slot] && !in_flight[slot]->is_complete()) {
        // A lost device never completes it either; report the loss instead of skipping forever.
        return device->status() == rhi::DeviceStatus::Lost ? FrameResult::DeviceLost
                                                           : FrameResult::Skipped;
    }
    in_flight[slot].reset();

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

    const rhi::RenderTargetDesc desc =
        detail::target_desc(swapchain->extent(), swapchain->format());
    auto commands = device->create_command_list();
    renderer->begin(*commands, *target, desc);
    sink->begin_text_frame();
    // Swapchain images are not preserved across presents, so a presented frame is a full redraw;
    // the damage plan only decided whether this frame happens at all.
    ui::paint::paint_scene(root, host->styles(), *sink);
    renderer->end();
    commands->submit_async();
    in_flight[slot] = std::move(commands);
    slot = (slot + 1) % detail::kFramesInFlight;
    ++stats.frames;

    const auto present = swapchain->present(*image);
    if (present || present.error() == rhi::SwapchainError::Suboptimal) {
        ++stats.presented;
    } else if (present.error() == rhi::SwapchainError::OutOfDate) {
        recreate_swapchain();
    } else if (present.error() == rhi::SwapchainError::DeviceLost) {
        return FrameResult::DeviceLost;
    }
    return FrameResult::Presented;
}

void WindowHost::Impl::apply_metrics(const hal::WindowMetrics& metrics) {
    if (!available) {
        return;
    }
    if (swapchain != nullptr) {
        swapchain->resize({metrics.physical_width, metrics.physical_height});
    }
    host->resize(static_cast<float>(metrics.logical_width),
                 static_cast<float>(metrics.logical_height), metrics.dpi_scale);
    loop->mark_render_dirty();
}

WindowFrameStats WindowHost::Impl::run(std::uint32_t max_frames) {
    stats = {};
    if (!available) {
        return stats;
    }
    stats.available = true;
    stats.device_state = WindowDeviceState::Ready;
    const bool force_frames = max_frames > 0;
    // A previous run detached the queue so a platform event could not outlive its pump, so a second
    // run() reattaches it on entry instead of leaving the window deaf forever.
    window->set_input_queue(&pump.queue());
    pacer.reset();
    // The attempt budget bounds one loop invocation: a caller that retries run() after a give-up
    // starts a fresh bounded recovery sequence (app/recovery_policy.hpp).
    RecoveryPolicy recovery_policy;

    bool closed = false;
    double last_tick = hal::steady_now_seconds();
    // A frame that keeps failing (a device that never recovers, a slot that never completes, a
    // swapchain that never yields an image) stops the run instead of spinning (ADR-021).
    FrameFailureGuard failures;
    std::array<hal::WindowEvent, detail::kMaxWindowEvents> events{};
    // Startup is a frame cause: the scene has no dirty flags until the first layout.
    loop->mark_render_dirty();

    while (!closed) {
        const std::size_t event_count = window->poll_events(events);
        for (std::size_t index = 0; index < event_count; ++index) {
            switch (events[index].type) {
            case hal::WindowEventType::Close:
                closed = true;
                break;
            case hal::WindowEventType::Resize:
            case hal::WindowEventType::DpiChange:
                apply_metrics(window->metrics());
                break;
            default:
                break;
            }
        }
        if (closed) {
            break;
        }

        const ui::InputPumpStats pumped = pump.pump(root, *loop);
        stats.input_events += static_cast<std::uint32_t>(pumped.consumed);
        if (subtree_dirty(root)) {
            loop->mark_render_dirty();
        }
        if (force_frames && stats.frames < max_frames) {
            loop->mark_render_dirty();
        }

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

        const double dt = now - last_tick;
        last_tick = now;
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

        const FrameResult outcome = render_and_present();
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
            // A busy slot may free on any GPU completion; retry on the bounded backoff.
            idle(kFrameRetryWaitSeconds);
            continue;
        }
        failures.succeeded();
        pacer.present(hal::steady_now_seconds());
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
    impl_ = std::make_unique<Impl>(root, std::move(sheet), std::move(options), scheduler);
}

WindowHost::~WindowHost() = default;

bool WindowHost::available() const {
    return impl_->available;
}

WindowFrameStats WindowHost::run(std::uint32_t max_frames) {
    return impl_->run(max_frames);
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
