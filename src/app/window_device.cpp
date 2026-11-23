// SPDX-License-Identifier: MIT
//
// `WindowDevice` (issue #228): the shared device, per-device caches and runtime loop. See
// docs/design/multi-window.md for the ownership table and the reasoning behind each row.

#include "tessera/app/idle_policy.hpp"
#include "tessera/app/window.hpp"
#include "tessera/app/window_device.hpp"
#include "tessera/hal/timer/steady_time.hpp"
#include "window_internal.hpp"

#include <cstdio>
#include <exception>
#include <utility>

namespace tessera::app {

WindowDevice::WindowDevice(const WindowDeviceOptions& options)
    : pacer_(runtime::FramePacerConfig{0.0, 0.0, false}) {
    device_ = detail::create_device(options.validation, options.surface, options.backend);
    if (device_ == nullptr) {
        return;
    }
    loop_ = std::make_unique<runtime::RuntimeLoop>(clock_);
    recovery_ = std::make_unique<runtime::DeviceRecoveryController>(*device_);
    loop_->set_device_recovery(recovery_.get());
}

WindowDevice::~WindowDevice() {
    // The loop borrows the recovery controller, so it is detached before the controller dies. A
    // window that outlives its group is a caller bug, but the detach keeps teardown from reading a
    // freed controller rather than turning that bug into a crash.
    if (loop_ != nullptr) {
        loop_->set_device_recovery(nullptr);
    }
    release_presentation();
}

void WindowDevice::set_completion_wake(runtime::CompletionQueue* completions) {
    if (completions == nullptr) {
        // Detach from every window before the wake dies: a window whose wait still joins a freed
        // descriptor is the one thing this must not leave behind.
        for (std::size_t index = 0; index < window_list_size_; ++index) {
            if (hal::Window* window = windows_[index]->window(); window != nullptr) {
                window->set_wake_source(nullptr);
            }
        }
        wake_.reset();
        return;
    }
    wake_ = hal::create_wake_source();
    if (wake_ == nullptr) {
        // The platform has no joinable wake primitive: the park keeps its bounded sleep, which is
        // the documented fallback, and the clamp stays the latency path there.
        return;
    }
    hal::WakeSource* source = wake_.get();
    // A wake source is a function pointer plus a context, so a lambda with no capture converts;
    // the completion queue's wake is called from any thread.
    completions->set_wake(
        [](void* context) { static_cast<hal::WakeSource*>(context)->wake(); }, source);
    for (std::size_t index = 0; index < window_list_size_; ++index) {
        if (hal::Window* window = windows_[index]->window(); window != nullptr) {
            window->set_wake_source(source);
        }
    }
}

bool WindowDevice::register_window(WindowHost& window) noexcept {
    if (window_list_size_ >= kMaxWindowsPerDevice) {
        return false;
    }
    // Idempotent: a host registers once in its constructor, and a second registration would make
    // `pump_windows_once` drive it twice per iteration.
    for (std::size_t index = 0; index < window_list_size_; ++index) {
        if (windows_[index] == &window) {
            return true;
        }
    }
    windows_[window_list_size_] = &window;
    ++window_list_size_;
    // A window that joins after `set_completion_wake` joins the same wake, so its park is not the
    // one place an async result still waits out the clamp (issue #842).
    if (wake_ != nullptr) {
        if (hal::Window* native = window.window(); native != nullptr) {
            native->set_wake_source(wake_.get());
        }
    }
    return true;
}

bool WindowDevice::unregister_window(WindowHost& window) noexcept {
    for (std::size_t index = 0; index < window_list_size_; ++index) {
        if (windows_[index] != &window) {
            continue;
        }
        // Preserve attach order, like the appfw registry: the walk order is the order a caller
        // registered in, which is what makes a two-window test's expected sequence readable.
        for (std::size_t move = index; move + 1 < window_list_size_; ++move) {
            windows_[move] = windows_[move + 1];
        }
        --window_list_size_;
        return true;
    }
    return false;
}

std::size_t WindowDevice::pump_windows_once() {
    if (loop_ == nullptr) {
        return 0;
    }
    // One tick for the whole group. `RuntimeLoop::tick()` *consumes* the render request, so a
    // second window that ticked the loop itself would see `should_render == false` and never paint
    // - which is exactly the bug the two-window pacing case caught. The tick is taken here and
    // handed to every window, so one tick can produce one frame per window.
    const runtime::RuntimeTick tick = loop_->tick();
    std::size_t presented = 0;
    // The size is read once: a window that closes during its own iteration unregisters itself, and
    // walking a shrinking list by index would skip the window that moved into the freed slot.
    const std::size_t count = window_list_size_;
    for (std::size_t index = 0; index < count; ++index) {
        // A window that unregistered itself during an earlier iteration of this same walk is gone
        // from the list, so re-read the slot rather than trusting the snapshot.
        if (index >= window_list_size_) {
            break;
        }
        WindowHost* const window = windows_[index];
        if (window == nullptr) {
            continue;
        }
        // `false`: the pacer gates ticks, not windows. It is advanced once below, after every
        // window has had its turn, so no window can consume the interval ahead of its siblings.
        if (window->pump_once(tick, false)) {
            ++presented;
        }
    }
    // One tick, one present interval. Advancing the pacer here rather than per window is what makes
    // two windows present in the same tick instead of the first one starving the second.
    if (presented > 0) {
        pacer_.present(hal::steady_now_seconds());
    }
    return presented;
}

double WindowDevice::next_wait_seconds() {
    // The nearer of the two deadlines the app has: the pacer's next present and the next timer.
    // `kNoDeadline` is infinity, so the minimum picks whichever is scheduled and stays infinite
    // when neither is - which the clamp below turns into the documented park.
    const double now = hal::steady_now_seconds();
    const runtime::PaceDecision pace = pacer_.ready(now);
    const double pacer_wait = pace.render_now ? 0.0 : pace.wait_seconds;
    const double timer_wait =
        loop_ != nullptr ? loop_->next_timer_wait_seconds() : runtime::kNoDeadline;
    const double wait = pacer_wait < timer_wait ? pacer_wait : timer_wait;
    if (!(wait > 0.0)) {
        // A due present or a due timer asks for 0, so the caller re-iterates immediately instead of
        // sleeping past the work.
        return 0.0;
    }
    // An uncapped pacer has no future deadline and a loop with no timer has none either, so a
    // settled app would otherwise park forever. The clamp is the same one `idle_policy.hpp` applies
    // to a single window, and it is what bounds how late a wake can be noticed.
    return wait < kMaxIdleWaitSeconds ? wait : kMaxIdleWaitSeconds;
}

WindowDevice::RunStats WindowDevice::run_windows(std::uint32_t max_frames) {
    RunStats stats;
    if (loop_ == nullptr) {
        return stats;
    }
    // Startup is a frame cause for every window, the same rule `run()` applies: the scene has no
    // dirty flags until the first layout.
    loop_->mark_render_dirty();
    for (;;) {
        if (stop_requested_) {
            // Consumed rather than merely observed: a request made before the run ends this one,
            // and a second run is not stopped by the first one's request.
            stop_requested_ = false;
            stats.stopped = true;
            break;
        }
        // A bounded run with nothing to paint returns at once rather than parking for frames that
        // cannot happen. An unbounded one keeps going: that is the tray app, which is waiting for a
        // window to be attached or a timer to fire.
        if (max_frames > 0 && window_list_size_ == 0) {
            break;
        }
        ++stats.iterations;
        stats.presented += pump_windows_once();
        if (max_frames > 0 && stats.presented >= max_frames) {
            break;
        }
        const double wait = next_wait_seconds();
        if (wait > 0.0) {
            ++stats.idle_waits;
            // Park on the group's own deadline. With no window there is no platform event wait to
            // join, but with a completion queue installed the wake is, so a worker's post ends the
            // park instead of the tray app waiting out the clamp (issue #842). Without a wake the
            // wait is the plain bounded sleep it always was.
            const double deadline = hal::steady_now_seconds() + wait;
            if (wake_ == nullptr) {
                hal::wait_until_seconds(deadline);
            } else if (wake_->wait_until(deadline)) {
                wake_->drain();
            }
        }
    }
    return stats;
}

bool WindowDevice::ensure_presentation(rhi::TextureFormat format, const WindowOptions& options) {
    if (device_ == nullptr) {
        return false;
    }
    if (renderer_ != nullptr) {
        // Two windows in one group must present through one format: the pipelines are created
        // against it, and a second format would need a second pipeline set, which is exactly the
        // duplication this type exists to remove. A mismatch is reported rather than silently
        // rebuilding the pipelines under the first window.
        if (format != format_) {
            std::fprintf(
                stderr, "TESSERA_WINDOW_FORMAT_MISMATCH: a window in this group presents through a "
                        "different swapchain format; the shared pipelines cannot serve both\n");
            return false;
        }
        return true;
    }
    format_ = format;
    has_format_ = true;

    // The sample count is resolved by the first window's AA policy and then shared, because the
    // pipeline, the renderer config and every window's offscreen frame target must agree on it.
    const std::uint32_t sample_count = presentation_sample_count_;
    if (sample_count > 1) {
        present_pipeline_ = detail::create_present_pipeline(*device_, format);
        if (present_pipeline_ == nullptr) {
            presentation_sample_count_ = 1;
        }
    }
    pipeline_ = detail::create_pipeline(*device_, format, presentation_sample_count_);
    if (pipeline_ == nullptr) {
        release_presentation();
        return false;
    }
    lcd_pipeline_ = detail::create_lcd_pipeline(*device_, format, presentation_sample_count_);
    detail::GlassPipelines glass = detail::create_glass_pipelines(*device_, format);
    if (glass.blur == nullptr || glass.composite == nullptr) {
        release_presentation();
        return false;
    }
    white_ = detail::create_white(*device_);
    if (white_ == nullptr) {
        release_presentation();
        return false;
    }
    glass_blur_pipeline_ = std::move(glass.blur);
    glass_composite_pipeline_ = std::move(glass.composite);

    rendering::gpu::Batched2DConfig renderer_config{4096, true, false};
    // The one number the refactor changes: one renderer serves every window in the group, and one
    // tick can call begin() once per window, so the frame ring must hold that many frames.
    renderer_config.frames_in_flight = renderer_frames_in_flight();
    renderer_config.sample_count = presentation_sample_count_;
    renderer_config.max_offscreen_targets = options.runtime.glass_max_blur_targets;
    renderer_config.glass.max_blur_radius = options.runtime.glass_max_blur_radius;
    renderer_config.glass.max_backdrop_area = options.runtime.glass_max_backdrop_area;
    renderer_config.cull_draws = options.runtime.cull_draws;

    renderer_ = std::make_unique<rendering::gpu::Batched2DRenderer>(
        *device_, *pipeline_, *white_, renderer_config,
        rendering::gpu::GlassPipelines{glass_blur_pipeline_.get(),
                                       glass_composite_pipeline_.get()});
    renderer_->set_lcd_pipeline(lcd_pipeline_.get());
    // The text stack owns the glyph atlas textures, so it is recreated with the device generation
    // instead of reusing an atlas whose images died with the old device.
    text_ = std::make_unique<detail::TextStack>(*device_, options.font_path);
    sink_ = std::make_unique<ui::paint::Batched2DSink>(*renderer_, text_->renderer.get(),
                                                       text_->shaper.get());
    return true;
}

void WindowDevice::release_presentation() {
    // Destruction order is the dependency order: the sink borrows the renderer and the text
    // renderer, and the renderer borrows the pipeline and the white texture.
    sink_.reset();
    renderer_.reset();
    text_.reset();
    white_.reset();
    pipeline_.reset();
    present_pipeline_.reset();
    lcd_pipeline_.reset();
    glass_blur_pipeline_.reset();
    glass_composite_pipeline_.reset();
    format_ = rhi::TextureFormat::R8G8B8A8Unorm;
    has_format_ = false;
    presentation_sample_count_ = 1;
}

bool WindowDevice::replace_device(const WindowDeviceOptions& options) {
    if (loop_ != nullptr) {
        loop_->set_device_recovery(nullptr);
    }
    recovery_.reset();
    release_presentation();
    try {
        device_ = detail::create_device(options.validation, options.surface, options.backend);
    } catch (const std::exception&) {
        // Backend factory calls throw only on contract violations (a lost/invalid device); a
        // recovery attempt reports a value instead of unwinding the driver (ADR-025 boundary).
        device_.reset();
    }
    if (device_ == nullptr) {
        return false;
    }
    recovery_ = std::make_unique<runtime::DeviceRecoveryController>(*device_);
    if (loop_ != nullptr) {
        loop_->set_device_recovery(recovery_.get());
    }
    return true;
}

} // namespace tessera::app
