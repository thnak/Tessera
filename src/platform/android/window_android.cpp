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
//   - It translates input through `AndroidInputTranslator` (android_input.cpp): the host calls
//     `translate_input` from its `onInputEvent` callback, and the translator pushes normalized
//     events into the queue `set_input_queue` installed. Historical/coalesced samples, tilt and
//     orientation are not carried yet (see the translator's header).
//   - It does NOT report insets, cutouts or fold state. Those come from `WindowInsets` and
//     `WindowMetricsCalculator` through JNI, which is a separate bridge
//     (docs/design/mobile-integration.md sections 3.5 and 3.6). The metrics carry the documented
//     zero-inset fallback, which is what every platform without a source reports.
//
// The surface lifetime rule is the important one: `ANativeWindow_acquire`/`release` are reference
// counted, and the platform may destroy the window while the app is backgrounded. The backend holds
// one reference for its own lifetime and revokes the borrowed `NativeSurface` when the host reports
// the surface is gone, so a renderer can never present into a dead window.

#include "android_input.hpp"
#include "android_metrics_translation.hpp"
#include "tessera/hal/surface.hpp"
#include "tessera/hal/timer/steady_time.hpp"
#include "tessera/hal/window.hpp"

#include <algorithm>
#include <android/input.h>
#include <android/native_window.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>

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
        // The platform thread writes `metrics_` through the setters below while the render thread
        // reads it here and in `poll_events`. Android delivers insets, cutouts and fold posture on
        // the Java UI thread, so this is a genuine cross-thread read, not a same-thread one: the
        // mutex is what makes it defined behaviour rather than a torn read of a large struct.
        const std::lock_guard lock(metrics_mutex_);
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
        // The events this backend produces are the ones the host reported through its setters. They
        // are queued here and coalesced like every other geometry event: a resize, a safe-area
        // change (insets or cutouts) and a fold change are each at most one pending event, so a
        // burst of platform callbacks between two ticks costs one event per kind.
        if (events.empty()) {
            return 0;
        }
        // One lock for the whole batch: the pending flags and the metrics snapshot they carry must
        // agree, or an event could report a fold the metrics no longer have.
        const std::lock_guard lock(metrics_mutex_);
        std::size_t written = 0;
        auto emit = [&](WindowEventType type, bool& pending) {
            if (!pending || written == events.size()) {
                return;
            }
            pending = false;
            WindowEvent event;
            event.type = type;
            event.metrics = metrics_;
            event.reason = EventReason::Unspecified;
            event.sequence = ++sequence_;
            events[written++] = event;
        };
        emit(WindowEventType::Resize, pending_resize_);
        emit(WindowEventType::SafeAreaChanged, pending_safe_area_);
        emit(WindowEventType::FoldChanged, pending_fold_);
        return written;
    }

    bool wait_for_events(double timeout_seconds) override {
        // Nothing to pump: Android delivers input on the UI thread through the host's own
        // callbacks, not through a queue this backend drains. Idle for the bounded timeout so a
        // caller that waits between ticks still throttles instead of spinning.
        if (std::isfinite(timeout_seconds) && timeout_seconds > 0.0) {
            wait_until_seconds(steady_now_seconds() + timeout_seconds);
        }
        return pending_resize_ || pending_safe_area_ || pending_fold_;
    }

    void set_input_queue(ui::InputQueue* queue) override {
        input_.set_queue(queue);
        input_.set_scale(dpi_scale_);
    }

    // Translates one platform input event into the queue. Called by the host from its
    // `onInputEvent` callback, on the thread that owns the window.
    bool translate_input(const void* event) override {
        return input_.translate(static_cast<const AInputEvent*>(event));
    }

    // Queues an event the host already translated (the Java MotionEvent path). The shell is a Java
    // activity, so touch arrives as a Java object and the host maps it; the queue lives here.
    bool post_input_event(const ui::InputEvent& event) override {
        return input_.push(event);
    }

    void show() override {
        const std::lock_guard lock(metrics_mutex_);
        visible_ = true;
        metrics_.minimized = false;
    }

    void hide() override {
        const std::lock_guard lock(metrics_mutex_);
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
    void set_surface_extent(std::uint32_t width_px, std::uint32_t height_px) override {
        if (width_px == 0 || height_px == 0) {
            return;
        }
        const std::lock_guard lock(metrics_mutex_);
        // The extent is recorded and the metrics are recomputed WITHOUT re-reading the
        // ANativeWindow: on a fold that switches to the cover display the adopted window keeps
        // reporting the old extent, so `refresh_metrics()` would overwrite the value the platform
        // just reported. That is why this path sets the fields directly.
        physical_width_ = width_px;
        physical_height_ = height_px;
        apply_geometry();
        pending_resize_ = true;
    }

    // The platform reported a new density scale (a display change, a user font/display setting).
    void set_dpi_scale(float scale) {
        if (scale <= 0.0f || scale == dpi_scale_) {
            return;
        }
        const std::lock_guard lock(metrics_mutex_);
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
        const std::lock_guard lock(metrics_mutex_);
        refresh_metrics();
        pending_resize_ = true;
    }

    // The platform reported new window insets (a bar visibility change, a rotation, a fold). The
    // values are physical pixels; the decode divides by the display scale.
    void set_insets(const android::PhysicalInsets& system_bars, const android::PhysicalInsets& ime,
                    const android::PhysicalInsets& gestures) {
        const std::lock_guard lock(metrics_mutex_);
        metrics_.insets = android::system_bar_insets(system_bars, dpi_scale_);
        metrics_.ime_insets = android::ime_insets(ime, dpi_scale_);
        metrics_.gesture_insets = android::gesture_insets(gestures, dpi_scale_);
        // The insets are now measured, so the "assumed" bits for them are cleared. The refresh and
        // work-area bits stay set: this backend still has no source for those.
        metrics_.assumed.work_area = 1;
        pending_safe_area_ = true;
    }

    // The platform reported the display cutouts (`DisplayCutout.getBoundingRects()`).
    void set_cutouts(std::span<const android::PhysicalRect> rects) {
        const std::lock_guard lock(metrics_mutex_);
        metrics_.cutouts = android::cutout_info(rects, dpi_scale_);
        pending_safe_area_ = true;
    }

    // The platform reported a fold posture (`FoldingFeature`).
    void set_fold(android::FoldPosture posture, android::FoldAxis axis,
                  const android::PhysicalRect& hinge) {
        const std::lock_guard lock(metrics_mutex_);
        metrics_.fold = android::fold_info(posture, axis, hinge, dpi_scale_);
        pending_fold_ = true;
    }

    // The HAL seam the JNI bridge calls (issue #189). It takes the platform's physical values and
    // forwards them to the decode above, so the bridge never needs to know the layout rules.
    void set_physical_insets(const PhysicalInsets& system_bars, const PhysicalInsets& ime,
                             const PhysicalInsets& gestures) override {
        android::PhysicalInsets bars{system_bars.left, system_bars.top, system_bars.right,
                                     system_bars.bottom};
        android::PhysicalInsets keyboard{ime.left, ime.top, ime.right, ime.bottom};
        android::PhysicalInsets gesture{gestures.left, gestures.top, gestures.right,
                                        gestures.bottom};
        set_insets(bars, keyboard, gesture);
    }

    void set_physical_cutouts(std::span<const PhysicalRect> rects) override {
        // The HAL type and the decode type are the same four numbers; the conversion is a copy so
        // the public header does not have to expose the decode header.
        std::array<android::PhysicalRect, kMaxCutouts> converted{};
        const std::size_t count = std::min(rects.size(), converted.size());
        for (std::size_t i = 0; i < count; ++i) {
            converted[i].left = rects[i].left;
            converted[i].top = rects[i].top;
            converted[i].right = rects[i].right;
            converted[i].bottom = rects[i].bottom;
        }
        set_cutouts(std::span<const android::PhysicalRect>(converted.data(), count));
    }

    // The fold posture the platform reported. The HAL enum and the decode enum are deliberately
    // separate: the HAL one is the platform's vocabulary, the decode one is the engine's, and the
    // mapping between them is the part worth testing.
    void set_physical_fold(const PhysicalFold& fold) override {
        android::FoldPosture posture = android::FoldPosture::None;
        switch (fold.posture) {
        case PhysicalFoldPosture::Flat:
            posture = android::FoldPosture::Flat;
            break;
        case PhysicalFoldPosture::HalfOpened:
            posture = android::FoldPosture::HalfOpened;
            break;
        case PhysicalFoldPosture::None:
            posture = android::FoldPosture::None;
            break;
        }
        android::FoldAxis axis = android::FoldAxis::None;
        switch (fold.axis) {
        case PhysicalFoldAxis::Horizontal:
            axis = android::FoldAxis::Horizontal;
            break;
        case PhysicalFoldAxis::Vertical:
            axis = android::FoldAxis::Vertical;
            break;
        case PhysicalFoldAxis::None:
            axis = android::FoldAxis::None;
            break;
        }
        android::PhysicalRect hinge{fold.hinge.left, fold.hinge.top, fold.hinge.right,
                                    fold.hinge.bottom};
        set_fold(posture, axis, hinge);
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
        apply_geometry();
    }

    // Recomputes the metrics from the recorded physical extent and scale, without touching the
    // ANativeWindow. Split out of `refresh_metrics` so the platform-reported extent path can reuse
    // it: that path must NOT re-read the window, which still reports the pre-fold size.
    void apply_geometry() {
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
    // Guards `metrics_`, the pending flags and the geometry fields the setters write. Android
    // delivers insets, cutouts and fold posture on the Java UI thread while the render thread reads
    // them from `metrics()` and `poll_events()`, so every access is a cross-thread one. The mutex
    // is held for one struct copy or one decode, never across a present or a platform call.
    mutable std::mutex metrics_mutex_;
    WindowMetrics metrics_{};
    SurfaceLifetime lifetime_;
    AndroidInputTranslator input_;
    std::uint64_t sequence_ = 0;
    bool visible_ = true;
    bool closed_ = false;
    bool pending_resize_ = false;
    bool pending_safe_area_ = false;
    bool pending_fold_ = false;
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
