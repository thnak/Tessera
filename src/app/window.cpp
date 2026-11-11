// SPDX-License-Identifier: MIT

#include "tessera/app/frame_guard.hpp"
#include "tessera/app/idle_policy.hpp"
#include "tessera/app/window.hpp"
#include "tessera/hal/timer/steady_time.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/hal/window_wait.hpp"
#include "tessera/rendering/rhi/swapchain.hpp"
#include "tessera/runtime/frame_pacer.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/runtime/steady_clock.hpp"
#include "tessera/ui/input.hpp"
#include "tessera/ui/input_pump.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "window_internal.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>

namespace tessera::app {
namespace {

namespace rhi = tessera::rendering::rhi;
namespace hal = tessera::hal;

constexpr std::size_t kFramesInFlight = 2;
constexpr std::size_t kMaxWindowEvents = 16;

bool subtree_dirty(const ui::Node& root) {
    return root.subtree_dirty() != ui::Dirty::Clean || root.dirty() != ui::Dirty::Clean;
}

} // namespace

struct WindowHost::Impl {
    Impl(ui::Node& root, ui::style::CompiledStyleSheet sheet, WindowOptions window_options,
         ui::UpdateScheduler* update_scheduler)
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

        const hal::WindowMetrics metrics = window->metrics();
        rhi::SwapchainDesc swap_desc;
        swap_desc.extent = {metrics.physical_width, metrics.physical_height};
        swap_desc.image_count = 2;
        swap_desc.vsync = options.vsync;
        swap_desc.native_window = window->native_handle().window;
        swap_desc.native_display = window->native_handle().display;
        auto created = device->create_swapchain(swap_desc);
        if (!created) {
            device->wait_idle();
            device.reset();
            window.reset();
            return;
        }
        swapchain = std::move(*created);

        pipeline = detail::create_pipeline(*device, swapchain->format());
        white = detail::create_white(*device);
        rendering::gpu::Batched2DConfig renderer_config{4096, true, false};
        renderer_config.frames_in_flight = static_cast<std::uint32_t>(kFramesInFlight);
        renderer = std::make_unique<rendering::gpu::Batched2DRenderer>(*device, *pipeline, *white,
                                                                       renderer_config);
        text = std::make_unique<detail::TextStack>(*device, options.font_path);
        sink = std::make_unique<ui::paint::Batched2DSink>(*renderer, text->renderer.get(),
                                                          text->shaper.get());

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

    ~Impl() {
        if (window != nullptr) {
            window->set_input_queue(nullptr);
        }
        // No device-wide wait_idle(): drain only the lists this loop submitted, with a hard
        // budget, and let the device's own destruction be the deferred full drain.
        release_in_flight();
    }

    // Parks on the bounded platform wait for at most `seconds` (already clamped by the caller).
    // Returns as soon as the window has an event and never sleeps past the deadline: the HAL wait
    // slices at 50 ms and re-checks. A zero budget still pumps the message queue without sleeping
    // so a close queued during a long idle is never missed.
    void idle(double seconds) {
        if (!(seconds > 0.0) || !std::isfinite(seconds)) {
            (void)window->wait_for_events(0.0);
            return;
        }
        (void)hal::wait_for_events_until(*window, hal::steady_now_seconds() + seconds);
    }

    // Teardown drain: poll the lists this loop submitted instead of calling a device-wide
    // wait_idle(). The total budget is hard, so a lost or hung device cannot stall shutdown; a list
    // still running after it is released to the RHI, which owns the command resources until the
    // completion fence signals. The swapchain's own teardown drain is bounded the same way.
    void release_in_flight() {
        constexpr double kDrainBudgetSeconds = 0.25;
        constexpr std::uint64_t kPollSliceMs = 1;
        const double deadline = hal::steady_now_seconds() + kDrainBudgetSeconds;
        for (auto& commands : in_flight) {
            while (commands != nullptr && !commands->is_complete() &&
                   hal::steady_now_seconds() < deadline) {
                (void)commands->wait(kPollSliceMs);
            }
            commands.reset();
        }
    }

    // Recreates the swapchain at the window's current physical size. OutOfDate means the surface
    // changed size; resize(extent()) would be the documented no-op and never clear the error.
    void recreate_swapchain() {
        const hal::WindowMetrics current = window->metrics();
        swapchain->resize({current.physical_width, current.physical_height});
    }

    // One rendered frame: reuse a free slot, acquire, paint the whole scene, submit async and
    // present. Returns false when the frame could not be produced (slot busy, no image).
    bool render_and_present() {
        // A slot whose GPU work is still running is skipped, never waited on: the UI thread must
        // not block on the GPU (ADR-021). The frame is retried on the next tick.
        if (in_flight[slot] && !in_flight[slot]->is_complete()) {
            return false;
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
            break;
        }
        if (!image) {
            return false;
        }
        rhi::RenderTarget* target = swapchain->image_target(*image);
        if (target == nullptr) {
            return false;
        }

        const rhi::RenderTargetDesc desc =
            detail::target_desc(swapchain->extent(), swapchain->format());
        auto commands = device->create_command_list();
        renderer->begin(*commands, *target, desc);
        sink->begin_text_frame();
        // Swapchain images are not preserved across presents, so a presented frame is a full
        // redraw; the damage plan only decided whether this frame happens at all.
        ui::paint::paint_scene(root, host->styles(), *sink);
        renderer->end();
        commands->submit_async();
        in_flight[slot] = std::move(commands);
        slot = (slot + 1) % kFramesInFlight;
        ++stats.frames;

        const auto present = swapchain->present(*image);
        if (present || present.error() == rhi::SwapchainError::Suboptimal) {
            ++stats.presented;
        } else if (present.error() == rhi::SwapchainError::OutOfDate) {
            recreate_swapchain();
        }
        return true;
    }

    void apply_metrics(const hal::WindowMetrics& metrics) {
        if (!available) {
            return;
        }
        swapchain->resize({metrics.physical_width, metrics.physical_height});
        host->resize(static_cast<float>(metrics.logical_width),
                     static_cast<float>(metrics.logical_height), metrics.dpi_scale);
        loop->mark_render_dirty();
    }

    WindowFrameStats run(std::uint32_t max_frames) {
        stats = {};
        if (!available) {
            return stats;
        }
        stats.available = true;
        const bool force_frames = max_frames > 0;
        // A previous run detached the queue so a platform event could not outlive its pump, so a
        // second run() reattaches it on entry instead of leaving the window deaf forever.
        window->set_input_queue(&pump.queue());
        pacer.reset();

        bool closed = false;
        double last_tick = hal::steady_now_seconds();
        // A frame that keeps failing (lost device, a slot that never completes, a swapchain that
        // never yields an image) stops the run instead of spinning or hanging (ADR-021).
        FrameFailureGuard failures;
        std::array<hal::WindowEvent, kMaxWindowEvents> events{};
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
            if (tick.device_blocked) {
                // No presents while lost/recovering. Poll recovery on a short bounded retry and
                // give up after a few ticks so a bounded run still terminates on a lost device.
                ++stats.idle_waits;
                if (failures.failed()) {
                    stats.stalled = true;
                    break;
                }
                idle(kFrameRetryWaitSeconds);
                continue;
            }
            if (!tick.should_render) {
                // Settled scene: do not begin a host frame at all, so no style, layout or paint
                // work runs. Park until the next scheduled present or a wake source.
                ++stats.idle_waits;
                idle(idle_wait_seconds(pacer.ready(hal::steady_now_seconds())));
                continue;
            }

            // A frame the runtime wants still respects the present-rate cap: idle to the pacer
            // deadline instead of rendering early and sleeping a fixed amount afterwards.
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
                // latched into a phantom render request, then idle without painting. The forced CI
                // path is the only caller allowed to paint an unchanged scene.
                host->end_frame();
                loop->clear_render_dirty();
                ++stats.idle_waits;
                idle(idle_wait_seconds(pacer.ready(hal::steady_now_seconds())));
                continue;
            }

            if (!render_and_present()) {
                ++stats.idle_waits;
                if (failures.failed()) {
                    stats.stalled = true;
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
            // Park until the next present is due (a zero budget only pumps the event queue when
            // the pacer is uncapped, so a continuous loop never spins on a timer).
            idle(pacer_wait_seconds(pacer.ready(hal::steady_now_seconds())));
        }

        window->set_input_queue(nullptr);
        release_in_flight();
        return stats;
    }

    ui::Node& root;
    WindowOptions options;
    ui::UpdateScheduler* scheduler = nullptr;
    bool available = false;

    std::unique_ptr<hal::Window> window;
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<rhi::Swapchain> swapchain;
    std::unique_ptr<rhi::Pipeline> pipeline;
    std::unique_ptr<rhi::Texture> white;
    std::unique_ptr<rendering::gpu::Batched2DRenderer> renderer;
    std::unique_ptr<detail::TextStack> text;
    std::unique_ptr<ui::paint::Batched2DSink> sink;
    runtime::SteadyClock clock;
    std::unique_ptr<runtime::RuntimeLoop> loop = std::make_unique<runtime::RuntimeLoop>(clock);
    // Present-rate cap and deadline source for the idle paths (0 = uncapped).
    runtime::FramePacer pacer;
    std::unique_ptr<AppHost> host;

    ui::InputPump pump;
    WindowFrameStats stats;
    std::array<std::unique_ptr<rhi::CommandList>, kFramesInFlight> in_flight;
    std::size_t slot = 0;
};

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

} // namespace tessera::app
