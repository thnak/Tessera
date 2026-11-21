// SPDX-License-Identifier: MIT
//
// Android window backend (issue #189).
//
// Android has no window manager a native process can create a window in: the OS owns the surface,
// and the app receives an `ANativeWindow*` from the platform (a `SurfaceView`/`TextureView` holder,
// or `NativeActivity`'s `ANativeWindow`). This backend therefore does NOT create a window; it
// *adopts* one. `create_android_window` takes the `ANativeWindow*` the host already owns and wraps
// it in the HAL contract, so the rest of the engine keeps seeing `WindowMetrics` and never a
// platform handle (ADR-020 rule 1).
//
// What this slice does and does not do:
//
//   - It reports the surface extent, the device scale and the window family, which is what the
//     renderer needs to size a swapchain and what layout needs to resolve a viewport.
//   - It does NOT translate input. `MotionEvent`/`KeyEvent` arrive through the Java `View` or the
//     `AInputQueue` the host owns, and wiring that into `ui::InputQueue` is the next slice
//     (docs/design/mobile-integration.md section 3.2). `set_input_queue` stores the queue so the
//     translation can be added without changing this contract.
//   - It does NOT report insets, cutouts or fold state. Those come from `WindowInsets` and
//     `WindowMetricsCalculator` through JNI, which is a separate bridge
//     (docs/design/mobile-integration.md sections 3.5 and 3.6). The metrics carry the documented
//     zero-inset fallback, which is what every platform without a source reports.
//
// The surface lifetime rule is the important one: `ANativeWindow_acquire`/`release` are reference
// counted, and the platform may destroy the window while the app is backgrounded. The backend holds
// one reference for its own lifetime and revokes the borrowed `NativeSurface` when the host reports
// the surface is gone, so a renderer can never present into a dead window.

#include "tessera/hal/surface.hpp"
#include "tessera/hal/timer/steady_time.hpp"
#include "tessera/hal/window.hpp"

#include <android/native_window.h>
#include <cmath>
#include <cstdint>
#include <memory>

namespace tessera::hal {
namespace {

// The Android density bucket the platform reports is a scale factor already (1.0 = mdpi), so the
// metrics carry it directly. `ANativeWindow_getWidth`/`Height` are physical pixels, which is what
// `physical_width`/`physical_height` mean; the logical size is the physical size divided by the
// scale, rounded to the nearest pixel so a 2.75x device does not lose a row to truncation.
std::uint32_t logical_from_physical(std::uint32_t physical, float scale) {
    if (scale <= 0.0f) {
        return physical;
    }
    const float logical = static_cast<float>(physical) / scale;
    return static_cast<std::uint32_t>(std::lround(logical));
}

class AndroidWindow final : public Window {
public:
    AndroidWindow(ANativeWindow* window, const WindowDesc& desc, float dpi_scale)
        : window_(window),
          desc_(desc),
          dpi_scale_(dpi_scale > 0.0f ? dpi_scale : 1.0f) {
        // One reference for this object's lifetime. The host keeps its own; releasing ours in the
        // destructor is what makes the window collectable when the app is done with it.
        ANativeWindow_acquire(window_);
        refresh_metrics();
    }

    ~AndroidWindow() override {
        // Revoke before releasing: a renderer holding a borrowed surface must see it invalid
        // before the handle can be reused by the platform.
        lifetime_.revoke();
        if (window_ != nullptr) {
            ANativeWindow_release(window_);
            window_ = nullptr;
        }
    }

    AndroidWindow(const AndroidWindow&) = delete;
    AndroidWindow& operator=(const AndroidWindow&) = delete;

    [[nodiscard]] WindowMetrics metrics() const override {
        return metrics_;
    }

    [[nodiscard]] WindowSystem window_system() const noexcept override {
        return WindowSystem::Android;
    }

    [[nodiscard]] NativeWindowHandle native_handle() const override {
        // Android has no separate display connection: the ANativeWindow* is the whole handle, and
        // `vkCreateAndroidSurfaceKHR` takes exactly it.
        return {nullptr, window_};
    }

    std::size_t poll_events(std::span<WindowEvent> events) override {
        // No event source is wired yet (see the file comment). The one event this backend can
        // produce on its own is a resize the host reported through `set_surface_extent`, which is
        // queued here and coalesced like every other geometry event.
        if (pending_resize_ && !events.empty()) {
            pending_resize_ = false;
            WindowEvent event;
            event.type = WindowEventType::Resize;
            event.metrics = metrics_;
            event.reason = EventReason::Unspecified;
            event.sequence = ++sequence_;
            events[0] = event;
            return 1;
        }
        return 0;
    }

    bool wait_for_events(double timeout_seconds) override {
        // Nothing to pump: Android delivers input on the UI thread through the host's own
        // callbacks, not through a queue this backend drains. Idle for the bounded timeout so a
        // caller that waits between ticks still throttles instead of spinning.
        if (std::isfinite(timeout_seconds) && timeout_seconds > 0.0) {
            wait_until_seconds(steady_now_seconds() + timeout_seconds);
        }
        return pending_resize_;
    }

    void set_input_queue(ui::InputQueue* queue) override {
        // Stored for the input-translation slice; nothing produces events yet.
        input_queue_ = queue;
    }

    void show() override {
        visible_ = true;
        metrics_.minimized = false;
    }

    void hide() override {
        visible_ = false;
        metrics_.minimized = true;
    }

    void request_redraw() override {
        // The host owns the frame loop; a redraw request is a no-op here because Android drives
        // frames from `Choreographer`, not from a window message.
    }

    void close() override {
        if (closed_) {
            return;
        }
        closed_ = true;
        lifetime_.revoke();
    }

    // --- Android-specific, called by the host that owns the surface -----------------------------

    // The platform reported a new surface extent (a rotation, a fold, a multi-window resize). The
    // metrics are recomputed and a coalesced Resize is queued.
    void set_surface_extent(std::uint32_t width_px, std::uint32_t height_px) {
        if (width_px == 0 || height_px == 0) {
            return;
        }
        physical_width_ = width_px;
        physical_height_ = height_px;
        refresh_metrics();
        pending_resize_ = true;
    }

    // The platform reported a new density scale (a display change, a user font/display setting).
    void set_dpi_scale(float scale) {
        if (scale <= 0.0f || scale == dpi_scale_) {
            return;
        }
        dpi_scale_ = scale;
        refresh_metrics();
        pending_resize_ = true;
    }

    // The host is handing over a different ANativeWindow (the old one was destroyed and a new
    // surface was created). The borrowed surface is revoked first, so a renderer that still holds
    // one cannot present into the dead window.
    void replace_surface(ANativeWindow* window) {
        if (window == window_) {
            return;
        }
        lifetime_.revoke();
        if (window_ != nullptr) {
            ANativeWindow_release(window_);
        }
        window_ = window;
        if (window_ != nullptr) {
            ANativeWindow_acquire(window_);
        }
        refresh_metrics();
        pending_resize_ = true;
    }

    // The surface the renderer borrows. Invalid once the window is closed or replaced.
    [[nodiscard]] NativeSurface surface() const {
        SurfaceDesc desc;
        desc.width_px = physical_width_;
        desc.height_px = physical_height_;
        desc.dpi_scale = dpi_scale_;
        desc.window_system = WindowSystem::Android;
        return NativeSurface::borrow(lifetime_, desc, native_handle());
    }

private:
    void refresh_metrics() {
        if (window_ != nullptr) {
            const int width = ANativeWindow_getWidth(window_);
            const int height = ANativeWindow_getHeight(window_);
            if (width > 0 && height > 0) {
                physical_width_ = static_cast<std::uint32_t>(width);
                physical_height_ = static_cast<std::uint32_t>(height);
            }
        }
        metrics_.physical_width = physical_width_;
        metrics_.physical_height = physical_height_;
        metrics_.logical_width = logical_from_physical(physical_width_, dpi_scale_);
        metrics_.logical_height = logical_from_physical(physical_height_, dpi_scale_);
        metrics_.dpi_scale = dpi_scale_;
        // The documented fallbacks: no inset, cutout, fold or refresh source is wired yet, so the
        // "assumed" bits stay set and a consumer cannot mistake them for measurements (ADR-023
        // rule 7).
        metrics_.assumed = {1, 1, 1, 1};
        metrics_.focused = visible_;
        metrics_.minimized = !visible_;
        metrics_.orientation = metrics_.logical_width >= metrics_.logical_height
                                   ? Orientation::Landscape
                                   : Orientation::Portrait;
    }

    ANativeWindow* window_ = nullptr;
    WindowDesc desc_;
    float dpi_scale_ = 1.0f;
    std::uint32_t physical_width_ = 0;
    std::uint32_t physical_height_ = 0;
    WindowMetrics metrics_{};
    SurfaceLifetime lifetime_;
    ui::InputQueue* input_queue_ = nullptr;
    std::uint64_t sequence_ = 0;
    bool visible_ = true;
    bool closed_ = false;
    bool pending_resize_ = false;
};

} // namespace

std::unique_ptr<Window> create_android_window(ANativeWindow* window, const WindowDesc& desc,
                                              float dpi_scale) {
    if (window == nullptr) {
        return nullptr;
    }
    return std::make_unique<AndroidWindow>(window, desc, dpi_scale);
}

// The HAL entry point every platform implements. On Android the caller must hand over the surface
// it already owns (`WindowDesc::surface`), because the OS owns the window and no descriptor can
// produce one; a null token is the documented "no surface yet" answer, which is what a host sees
// before its `SurfaceView` holder has a surface.
std::unique_ptr<Window> create_window(const WindowDesc& desc) {
    if (!desc.surface.valid()) {
        return nullptr;
    }
    return create_android_window(static_cast<ANativeWindow*>(desc.surface.token), desc,
                                 desc.dpi_scale);
}

// The base-class default for a platform with no event source of its own. It lives here rather than
// only in window_stub.cpp because `Window::wait_for_events` is the class's key function: the
// translation unit that defines it is the one that emits `typeinfo for tessera::hal::Window`, and
// without it a shared library that links this backend fails with
// "undefined symbol: typeinfo for tessera::hal::Window". Android does not compile window_stub.cpp
// (it has a real backend), so the definition has to be here too.
bool Window::wait_for_events(double timeout_seconds) {
    if (std::isfinite(timeout_seconds) && timeout_seconds > 0.0) {
        wait_until_seconds(steady_now_seconds() + timeout_seconds);
    }
    return false;
}

} // namespace tessera::hal
