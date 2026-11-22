// SPDX-License-Identifier: MIT
//
// `WindowDevice` (issue #228): the shared device, per-device caches and runtime loop. See
// docs/design/multi-window.md for the ownership table and the reasoning behind each row.

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
