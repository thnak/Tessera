// SPDX-License-Identifier: MIT

#include "tessera/app/window.hpp"
#include "tessera/hal/timer/steady_time.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/rendering/rhi/swapchain.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/runtime/steady_clock.hpp"
#include "tessera/ui/input.hpp"
#include "tessera/ui/input_pump.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "window_internal.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>

namespace tessera::app {
namespace {

namespace rhi = tessera::rendering::rhi;
namespace hal = tessera::hal;

constexpr std::size_t kFramesInFlight = 2;
constexpr std::uint32_t kSlotWaitMs = 1000;
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
          scheduler(update_scheduler) {
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
        if (device != nullptr) {
            device->wait_idle();
        }
    }

    // One rendered frame: wait for the reused slot, acquire, paint the whole scene, submit async
    // and present. Returns false when the frame could not be produced (slot busy, no image).
    bool render_and_present() {
        if (in_flight[slot] && !in_flight[slot]->wait(kSlotWaitMs)) {
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
                swapchain->resize(swapchain->extent());
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
            swapchain->resize(swapchain->extent());
        }
        return true;
    }

    WindowFrameStats run(std::uint32_t max_frames) {
        stats = {};
        if (!available) {
            return stats;
        }
        stats.available = true;
        const bool force_frames = max_frames > 0;

        bool closed = false;
        double last_tick = hal::steady_now_seconds();
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
                case hal::WindowEventType::DpiChange: {
                    const hal::WindowMetrics metrics = window->metrics();
                    swapchain->resize({metrics.physical_width, metrics.physical_height});
                    host->resize(static_cast<float>(metrics.logical_width),
                                 static_cast<float>(metrics.logical_height), metrics.dpi_scale);
                    loop->mark_render_dirty();
                    break;
                }
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
            if (!tick.should_render) {
                ++stats.idle_waits;
                window->wait_for_events(0.25);
                continue;
            }

            const double now = hal::steady_now_seconds();
            const double dt = now - last_tick;
            last_tick = now;
            const HostFrame frame = host->begin_frame(dt, tick.alpha);
            if (!frame.frame_due && !force_frames) {
                loop->clear_render_dirty();
                ++stats.idle_waits;
                window->wait_for_events(0.02);
                continue;
            }

            if (!render_and_present()) {
                ++stats.idle_waits;
                window->wait_for_events(0.002);
                continue;
            }
            host->end_frame();
            loop->clear_render_dirty();
            if (force_frames && stats.frames >= max_frames) {
                break;
            }
            window->wait_for_events(0.001);
        }

        window->set_input_queue(nullptr);
        device->wait_idle();
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
    std::unique_ptr<AppHost> host;

    ui::InputPump pump;
    WindowFrameStats stats;
    std::array<std::unique_ptr<rhi::CommandList>, kFramesInFlight> in_flight;
    std::size_t slot = 0;
};

WindowHost::WindowHost(ui::Node& root, ui::style::CompiledStyleSheet sheet, WindowOptions options,
                       ui::UpdateScheduler* scheduler)
    : impl_(std::make_unique<Impl>(root, std::move(sheet), std::move(options), scheduler)) {}

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
