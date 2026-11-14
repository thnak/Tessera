// SPDX-License-Identifier: MIT
//
// Wayland window backend: xdg-shell toplevel behind hal::Window (issue #155).
//
// Scope of this slice: create a toplevel, follow the configure/ack handshake, report the compositor
// owned size through WindowMetrics, normalize the documented lifecycle and resize events, expose
// the wl_display*/wl_surface* pair the Vulkan KHR_wayland_surface path presents through, and close.
//
// Deliberately NOT in this slice (each is its own follow-up item):
//  - input: no wl_seat is bound, so set_input_queue() keeps the no-op default. The follow-up slice
//    owns wl_seat + wl_pointer/wl_keyboard/wl_touch normalization, pointer capture and the motion
//    coalescing rules in docs/design/input.md.
//  - text input: no zwp_text_input_v3 / IME, so no Text/Composition events.
//  - fractional scale / wl_output: no wl_output is bound and no wp_fractional_scale_v1 /
//    wp_viewporter is used. display_refresh_hz() stays kUnknownRefreshHz and dpi_scale stays 1.0;
//    both are documented "unknown"/integer-scale answers, not guesses. Per-window scale-aware
//    resizing is the same item.
//  - server-side decorations (xdg-decoration) and popups (xdg_popup): the window is a plain
//    xdg_toplevel; a compositor draws its own frame when it wants one.
//  - full-screen requests: supports_fullscreen() stays false and set_fullscreen() keeps the base
//    no-op. A compositor that puts the toplevel in fullscreen on its own is still reported through
//    WindowMetrics::fullscreen, because that field is the resolved state, not the last request.
//  - wl_shm: never bound. A Wayland surface maps when a buffer is attached, and on this backend the
//    first presented Vulkan image is that buffer (VK_KHR_wayland_surface attaches the swapchain
//    image to the very same wl_surface). show()/hide() therefore only track the logical visibility;
//    hide() actively unmaps with a null-buffer attach. No extra placeholder buffer is allocated.
//
// Two translation units share the work: window_wayland_shell.cpp owns the xdg-shell role stack and
// its configure sequence, window_wayland_dispatch.cpp owns the bounded libwayland read loop, and
// this file owns the hal::Window shape (metrics, normalized events, visibility, close).

#include "tessera/hal/timer/steady_time.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/hal/window_backdrop.hpp"
#include "window_event_queue.hpp"
#include "window_wayland.hpp"
#include "window_wayland_dispatch.hpp"
#include "window_wayland_internal.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <thread>
#include <wayland-client.h>

namespace tessera::hal {
namespace {

using detail::EventQueue;
using detail::WaylandConfigureState;
using detail::WaylandConnection;
using detail::WaylandShell;

// The whole bring-up (registry globals + the initial configure sequence) is bounded: a compositor
// that never answers fails the window instead of parking the UI thread (ADR-021). Weston answers in
// a few milliseconds; two seconds is generous for a loaded machine and still a bounded wait.
constexpr double kBringUpTimeoutSeconds = 2.0;

// One diagnostic for every unsupported Wayland bring-up (the caller may still fall back to X11).
void report_unsupported(const char* reason) {
    std::fprintf(stderr, "TESSERA_WINDOW_WAYLAND_UNSUPPORTED: %s\n", reason);
}

} // namespace

class WaylandWindow final : public Window {
public:
    WaylandWindow() = default;

    ~WaylandWindow() override;

    WaylandWindow(const WaylandWindow&) = delete;
    WaylandWindow& operator=(const WaylandWindow&) = delete;

    bool create(const WindowDesc& desc);

    [[nodiscard]] WindowMetrics metrics() const override;
    [[nodiscard]] WindowSystem window_system() const noexcept override {
        return WindowSystem::Wayland;
    }
    [[nodiscard]] NativeWindowHandle native_handle() const override {
        return {connection_.display(), surface_};
    }
    std::size_t poll_events(std::span<WindowEvent> events) override;
    bool wait_for_events(double timeout_seconds) override;
    // Input is a separate follow-up slice (see the file header): no wl_seat is bound, so the base
    // no-op is the honest implementation rather than a half-translated queue.
    void set_input_queue(ui::InputQueue* /*queue*/) override {}
    void show() override;
    void hide() override;
    void request_redraw() override;
    void close() override;
    void set_caption_regions(std::span<const CaptionRegion> regions) override;

private:
    // Dispatches already-queued protocol events and normalizes them into `events_`.
    void pump();
    // Publishes the newest configure sequence into the metrics/state and queues the normalized
    // events. Runs from pump(), outside the protocol callbacks.
    void apply_configure(const WaylandConfigureState& configure);
    void enqueue(WindowEventType type);
    void request_close();
    [[nodiscard]] bool has_pending_events() const;
    // One bounded bring-up slice. False when the connection ended or the deadline passed.
    [[nodiscard]] bool wait_once(double deadline);
    [[nodiscard]] std::uint32_t resolved_width() const;
    [[nodiscard]] std::uint32_t resolved_height() const;

    detail::WaylandConnection connection_;
    detail::WaylandShell shell_;
    wl_surface* surface_ = nullptr;
    std::thread::id owner_thread_ = std::this_thread::get_id();
    EventQueue events_;
    // Client-chosen size: the compositor's configure falls back to it when it delegates the size
    // (a zero width/height in xdg_toplevel.configure).
    std::uint32_t preferred_width_ = 0;
    std::uint32_t preferred_height_ = 0;
    std::uint32_t configured_width_ = 0;
    std::uint32_t configured_height_ = 0;
    std::array<CaptionRegion, kMaxCaptionRegions> caption_regions_{};
    std::uint8_t caption_region_count_ = 0;
    BackdropResolution backdrop_{};
    bool configured_ = false;
    bool initializing_ = true;
    bool mapped_ = false;
    bool focused_ = false;
    bool occluded_ = false;
    bool fullscreen_ = false;
    bool redraw_pending_ = false;
    bool close_posted_ = false;
    bool close_requested_ = false;
    bool close_delivered_ = false;
};

bool WaylandWindow::wait_once(double deadline) {
    if (!connection_.wait_once(deadline)) {
        return false;
    }
    if (shell_.consume_close_request()) {
        request_close();
    }
    // Any listener the dispatch invoked recorded an xdg-shell configure sequence; publish it now,
    // outside the protocol callback, so no user-visible work happens on the compositor's stack.
    if (const WaylandConfigureState* configure = shell_.consume_configure()) {
        apply_configure(*configure);
    }
    return true;
}

bool WaylandWindow::create(const WindowDesc& desc) {
    if (desc.width == 0 || desc.height == 0) {
        return false;
    }
    preferred_width_ = desc.width;
    preferred_height_ = desc.height;
    if (!connection_.connect()) {
        report_unsupported("cannot connect to the Wayland compositor (WAYLAND_DISPLAY)");
        return false;
    }
    if (!shell_.connect(connection_.display())) {
        report_unsupported("cannot create the wl_registry");
        shell_.destroy();
        connection_.disconnect();
        return false;
    }

    // Bring-up is one bounded loop: wait for the wl_compositor/xdg_wm_base globals, then create the
    // role stack and wait for the initial configure sequence that follows the role commit. A
    // compositor without xdg-shell never becomes ready and is reported once.
    const double deadline = steady_now_seconds() + kBringUpTimeoutSeconds;
    while (!shell_.globals_ready()) {
        if (!wait_once(deadline) || steady_now_seconds() >= deadline) {
            report_unsupported("the compositor has no usable wl_compositor/xdg_wm_base global");
            shell_.destroy();
            connection_.disconnect();
            return false;
        }
    }
    if (!shell_.create_roles(desc.title.c_str())) {
        report_unsupported("cannot create the xdg_toplevel role");
        shell_.destroy();
        connection_.disconnect();
        return false;
    }
    surface_ = shell_.surface();
    while (!shell_.configured()) {
        if (!wait_once(deadline) || steady_now_seconds() >= deadline) {
            report_unsupported("the compositor sent no xdg_surface configure");
            shell_.destroy();
            surface_ = nullptr;
            connection_.disconnect();
            return false;
        }
    }

    // The OS backdrop resolution (glass-v1.md section 6): a Wayland surface can composite per-pixel
    // alpha; no ext-background-effect-v1 global is bound this slice, so a named blur material falls
    // back and `auto` resolves to no material (both are the documented outcomes).
    BackdropCapabilities capabilities;
    capabilities.platform = BackdropPlatform::Wayland;
    capabilities.per_pixel_alpha = true;
    backdrop_ = resolve_window_backdrop(desc.backdrop, desc.transparent, capabilities);

    // Creation-time state is state, not events: like the X11 backend, drain what the configure
    // handshake produced and start the run with a clean queue.
    pump();
    initializing_ = false;
    events_.clear();
    redraw_pending_ = false;
    mapped_ = desc.visible;
    return true;
}

WaylandWindow::~WaylandWindow() {
    if (connection_.display() == nullptr) {
        return;
    }
    // The connection and every proxy belong to the creating thread; a foreign-thread destructor
    // detaches and leaks them rather than racing the owning thread's dispatch loop.
    if (std::this_thread::get_id() != owner_thread_) {
        connection_.leak();
        return;
    }
    shell_.destroy();
    surface_ = nullptr;
    connection_.disconnect();
}

std::uint32_t WaylandWindow::resolved_width() const {
    return configured_width_ != 0 ? configured_width_ : preferred_width_;
}

std::uint32_t WaylandWindow::resolved_height() const {
    return configured_height_ != 0 ? configured_height_ : preferred_height_;
}

WindowMetrics WaylandWindow::metrics() const {
    WindowMetrics metrics;
    if (connection_.display() == nullptr) {
        metrics.focused = false;
        return metrics;
    }
    // Wayland logical pixels at integer scale 1.0: no wl_output is bound, so no scale source exists
    // and physical == logical. Documented limitation of this slice (integer scale is a follow-up).
    metrics.logical_width = resolved_width();
    metrics.logical_height = resolved_height();
    metrics.physical_width = metrics.logical_width;
    metrics.physical_height = metrics.logical_height;
    metrics.dpi_scale = 1.0f;
    metrics.focused = focused_;
    metrics.occluded = occluded_;
    metrics.fullscreen = fullscreen_;
    metrics.backdrop = backdrop_.material;
    metrics.transparent = backdrop_.transparent;
    metrics.backdrop_fallback = backdrop_.fallback;
    metrics.caption_region_count = caption_region_count_;
    metrics.caption_regions = caption_regions_;
    // xdg-shell has no minimized state (a compositor unmaps or suspends instead), so `minimized`
    // stays false and a suspended toplevel is reported as occluded.
    return metrics;
}

void WaylandWindow::enqueue(WindowEventType type) {
    if (initializing_) {
        return;
    }
    WindowEvent event;
    event.type = type;
    event.metrics = metrics();
    events_.push(event);
}

void WaylandWindow::request_close() {
    if (close_requested_) {
        return;
    }
    close_requested_ = true;
    close_delivered_ = false;
    enqueue(WindowEventType::Close);
}

void WaylandWindow::apply_configure(const WaylandConfigureState& configure) {
    // The compositor owns the size: a positive configure wins, a zero one delegates the choice to
    // the client, which is what `preferred_width_/height_` remember.
    const std::uint32_t width =
        configure.width > 0 ? static_cast<std::uint32_t>(configure.width) : preferred_width_;
    const std::uint32_t height =
        configure.height > 0 ? static_cast<std::uint32_t>(configure.height) : preferred_height_;
    const bool size_changed =
        !configured_ || width != configured_width_ || height != configured_height_;
    configured_ = true;
    configured_width_ = width;
    configured_height_ = height;
    if (size_changed) {
        enqueue(WindowEventType::Resize);
    }
    if (configure.activated != focused_) {
        focused_ = configure.activated;
        enqueue(focused_ ? WindowEventType::Focus : WindowEventType::Blur);
    }
    if (configure.suspended != occluded_) {
        occluded_ = configure.suspended;
        // "suspended" is the compositor's "not being repainted" state: another window covers this
        // one, the outputs are off, or the screen is locked. It pauses the frame loop and resumes
        // it on the cleared state, which is what Occluded/Restored mean to the runtime.
        enqueue(occluded_ ? WindowEventType::Occluded : WindowEventType::Restored);
    }
    fullscreen_ = configure.fullscreen;
}

void WaylandWindow::pump() {
    if (!connection_.pump()) {
        request_close();
        return;
    }
    // A lost connection is a Close: the caller must not keep polling a dead compositor.
    if (connection_.lost()) {
        request_close();
    }
}

bool WaylandWindow::has_pending_events() const {
    return events_.count() > 0 || (close_requested_ && !close_delivered_) || redraw_pending_;
}

std::size_t WaylandWindow::poll_events(std::span<WindowEvent> events) {
    pump();
    // The dispatch loop invokes listeners; drain any configure sequence it published.
    if (const WaylandConfigureState* configure = shell_.consume_configure()) {
        apply_configure(*configure);
    }
    const std::size_t copied = events_.copy_to(events);
    for (std::size_t i = 0; i < copied; ++i) {
        if (events[i].type == WindowEventType::Close) {
            close_delivered_ = true;
        }
    }
    // A redraw wake is consumed by the poll that observes it; the runtime draws after polling.
    redraw_pending_ = false;
    return copied;
}

bool WaylandWindow::wait_for_events(double timeout_seconds) {
    // The connection and the event queue belong to the creating thread.
    if (std::this_thread::get_id() != owner_thread_ || connection_.display() == nullptr) {
        return false;
    }
    const double timeout =
        std::isfinite(timeout_seconds) && timeout_seconds > 0.0 ? timeout_seconds : 0.0;
    const double deadline = steady_now_seconds() + timeout;
    for (;;) {
        pump();
        if (const WaylandConfigureState* configure = shell_.consume_configure()) {
            apply_configure(*configure);
        }
        if (has_pending_events()) {
            return true;
        }
        if (seconds_until(steady_now_seconds(), deadline) <= 0.0) {
            return false;
        }
        if (!wait_once(deadline)) {
            return has_pending_events();
        }
    }
}

void WaylandWindow::show() {
    if (connection_.display() == nullptr || surface_ == nullptr || mapped_ ||
        std::this_thread::get_id() != owner_thread_) {
        return;
    }
    // Logical visibility: the surface maps when the first frame is presented, because the Vulkan
    // swapchain image is the buffer this surface gets (no wl_shm placeholder buffer is allocated).
    mapped_ = true;
    wl_surface_commit(surface_);
    connection_.pump();
}

void WaylandWindow::hide() {
    if (connection_.display() == nullptr || surface_ == nullptr || !mapped_ ||
        std::this_thread::get_id() != owner_thread_) {
        return;
    }
    // A null-buffer attach unmaps the surface and destroys the presented content, which is the
    // documented Wayland "hidden window" state.
    wl_surface_attach(surface_, nullptr, 0, 0);
    wl_surface_commit(surface_);
    connection_.pump();
    mapped_ = false;
    focused_ = false;
    redraw_pending_ = false;
}

void WaylandWindow::request_redraw() {
    if (connection_.display() == nullptr || surface_ == nullptr || !mapped_ ||
        std::this_thread::get_id() != owner_thread_) {
        // A hidden window has nothing to repaint; the first presented frame maps the surface.
        return;
    }
    // Wayland has no "invalidate this rectangle" request: the redraw wake is the normalized
    // redraw_pending_ flag read by poll_events()/wait_for_events(), and the runtime draws a frame.
    redraw_pending_ = true;
}

void WaylandWindow::close() {
    if (connection_.display() == nullptr || close_posted_ ||
        std::this_thread::get_id() != owner_thread_) {
        return;
    }
    // xdg_toplevel has no close request: a graceful close is the app asking the window to shut
    // down, and xdg_toplevel.close from the compositor takes the same path.
    close_posted_ = true;
    request_close();
}

void WaylandWindow::set_caption_regions(std::span<const CaptionRegion> regions) {
    caption_regions_ = make_caption_regions(regions, caption_region_count_);
    // Stored and reported through the metrics for the engine and the deterministic dump. Wayland
    // server-side decorations are the compositor's (xdg-decoration is not bound this slice), so the
    // platform's own chrome and hit test stay.
}

std::unique_ptr<Window> detail::create_wayland_window(const WindowDesc& desc) {
    auto window = std::make_unique<WaylandWindow>();
    if (!window->create(desc)) {
        return nullptr;
    }
    return window;
}

} // namespace tessera::hal
