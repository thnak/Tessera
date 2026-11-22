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
                       WindowOptions window_options, ui::UpdateScheduler* update_scheduler,
                       WindowDevice& group_ref, WindowHost& owner_ref)
    : root(root),
      options(std::move(window_options)),
      scheduler(update_scheduler),
      group(&group_ref),
      owner(&owner_ref) {
    // The shared device, loop and pacer are views into the group (issue #228). A group with no
    // device leaves every view null, which is the "no usable device" path the rest of the driver
    // already handles.
    device = group->device_or_null();
    loop = group->available() ? &group->loop() : nullptr;
    pacer = group->available() ? &group->pacer() : nullptr;
    recovery = group->recovery();
    if (loop == nullptr || device == nullptr) {
        return;
    }
    group->attach_window();
    // The pacer is app-scoped: the first window seeds it from its own options, and a later window
    // adopts the group's existing rate rather than resetting it. That is what keeps two windows
    // from drifting into two pacing regimes (docs/design/multi-window.md section 5).
    if (group->window_count() == 1) {
        pacer->update(runtime::FramePacerConfig{options.max_fps, options.refresh_hz, false},
                      hal::steady_now_seconds());
    }
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
    // A platform that cannot create a window on its own (Android) needs the surface the caller
    // already owns. Null everywhere else, which is what every existing caller keeps.
    desc.surface = options.surface;
    desc.dpi_scale = options.dpi_scale;
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
    // The device is the group's (issue #228): a window never creates one, so two windows cannot end
    // up with two devices. The window tells the device which window system it presents through
    // (issue #155) only when the group was created without one.
    if (device == nullptr) {
        window.reset();
        return;
    }
    if (!create_presentation()) {
        release_presentation();
        window.reset();
        return;
    }

    const hal::WindowMetrics metrics = window->metrics();
    // The lifecycle seam's baselines come from the window the platform just created: a window
    // created visible and focused is not a transition, so its first snapshot forwards nothing.
    seed_lifecycle_baseline(metrics);
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
    config.text_measurer = detail::layout_measurer(options.text_measurer, *text());
    // The OS appearance (issue #187) is read once from the platform window, which is the one
    // normalized source for both preferences on every backend: Windows answers from
    // SPI_GETCLIENTAREAANIMATION + AppsUseLightTheme, Linux from the settings portal (or the GTK
    // fallback) through its watcher. A platform with neither returns the documented fallback with
    // both assumptions set, so this never blocks and never guesses.
    const hal::AppearancePreference appearance = window->appearance();
    config.appearance = appearance;
    config.reduced_motion = appearance.reduced_motion || system_prefers_reduced_motion();
    config.pipeline = window_pipeline_config(options.runtime);
    config.adaptive_quality = window_adaptive_quality_policy(options.runtime, options.refresh_hz);
    // ADR-041's round budget (`layoutObservationMaxRounds`): the config loader already clamped it
    // into 1..16, and the settle component clamps again so a hand-built HostConfig is safe too.
    config.layout_observation_max_rounds =
        static_cast<std::uint32_t>(options.runtime.layout_observation_max_rounds);
    // The startup preference is recorded once so a later broadcast only moves the host when the
    // value actually changed (Win32 broadcasts settings for unrelated changes too).
    motion = MotionPreference(config.reduced_motion);
    host = std::make_unique<AppHost>(root, std::move(sheet), *loop, config);
    // The first insets apply at once, so a SafeArea never animates in from zero on startup.
    host->set_window_metrics(metrics);
    // Overlays see input first: Escape and back dismiss, a press outside closes popovers.
    pump.set_input_filter(
        [](void* user, ui::Node&, const ui::InputEvent& event) {
            return static_cast<AppHost*>(user)->handle_overlay_input(event);
        },
        host.get());
    if (scheduler != nullptr) {
        host->set_update_scheduler(scheduler);
    }
    window->set_input_queue(&pump.queue());
    // A platform can create the window already occluded or minimized; start paused in that case so
    // the first iteration does not acquire and present a frame the user cannot see. The policy can
    // disable that pause (`pauseWhenOccluded=false`), in which case the window keeps rendering.
    //
    // A window created with `visible = false` is deliberately *not* paused: that is the
    // offscreen/CI pattern, where the app paints into a window the user never sees, and it must
    // keep presenting. The tray app's zero-present state comes from `set_visible(false)` instead
    // (issue #181).
    metrics_paused_ = pause_policy.pauses(metrics.occluded, metrics.minimized);
    paused_ = metrics_paused_;
    last_tick_seconds_ = hal::steady_now_seconds();
    // Accessibility last: it needs the AppHost's computed-style provider for its snapshot source.
    // A failed start reports one diagnostic and leaves the window fully functional.
    start_accessibility();
    available = true;
    // The window joins the group's list (issue #180), so one loop can drive every window in the
    // group. Registration is last: a window that failed to build never joins the list, and a walk
    // therefore never has to skip a half-constructed host.
    (void)group->register_window(*owner);
}

WindowHost::Impl::~Impl() {
    // The accessibility controller goes first: it uninstalls the UIA root and joins the bridge or
    // AT-SPI session while the platform window (and its native handle) is still alive.
    accessibility.reset();
    if (window != nullptr) {
        window->set_input_queue(nullptr);
    }
    // No device-wide wait_idle(): drain only the lists this loop submitted, with a hard budget, and
    // let the device's own destruction be the deferred full drain.
    release_in_flight(detail::kDrainBudgetSeconds);
    // The device, loop and recovery controller belong to the group (issue #228), so this host only
    // detaches from it. A window closing must not tear down the device its siblings present
    // through.
    if (group != nullptr) {
        (void)group->unregister_window(*owner);
        group->detach_window();
    }
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

WindowHost::WindowHost(ui::Node& root, ui::style::CompiledStyleSheet sheet, WindowOptions options,
                       ui::UpdateScheduler* scheduler)
    : impl_(nullptr) {
    // Validate before the group is constructed: a negative or non-finite cap would throw from a
    // member initializer where the message is opaque.
    if (!std::isfinite(options.max_fps) || options.max_fps < 0.0) {
        throw std::invalid_argument("tessera app window: max_fps must be finite and non-negative");
    }
    if (!std::isfinite(options.refresh_hz) || options.refresh_hz < 0.0) {
        throw std::invalid_argument("tessera app window: refresh_hz must be finite and >= 0");
    }
    // The single-window path (issue #228): a private one-window group, built from the fields of
    // `options` that describe the device. This is exactly the pre-refactor behaviour, which is what
    // keeps every existing caller and test byte-identical. `Auto` keeps the environment-based
    // surface-family resolution the old per-window `create_device` call used.
    WindowDeviceOptions device_options;
    device_options.validation = options.validation;
    device_options.surface = rhi::SurfacePlatform::Auto;
    device_options.font_path = options.font_path;
    auto group = std::make_unique<WindowDevice>(device_options);
    impl_ = std::make_unique<Impl>(root, std::move(sheet), std::move(options), scheduler, *group,
                                   *this);
    impl_->owned_group = std::move(group);
}

WindowHost::WindowHost(ui::Node& root, ui::style::CompiledStyleSheet sheet, WindowOptions options,
                       WindowDevice& group, ui::UpdateScheduler* scheduler)
    : impl_(nullptr) {
    if (!std::isfinite(options.max_fps) || options.max_fps < 0.0) {
        throw std::invalid_argument("tessera app window: max_fps must be finite and non-negative");
    }
    if (!std::isfinite(options.refresh_hz) || options.refresh_hz < 0.0) {
        throw std::invalid_argument("tessera app window: refresh_hz must be finite and >= 0");
    }
    impl_ =
        std::make_unique<Impl>(root, std::move(sheet), std::move(options), scheduler, group, *this);
}

WindowHost::~WindowHost() = default;

bool WindowHost::available() const {
    return impl_->available;
}

hal::Window* WindowHost::window() const {
    return impl_->window.get();
}

WindowFrameStats WindowHost::run(std::uint32_t max_frames) {
    return impl_->run(max_frames);
}

ui::FocusManager& WindowHost::focus_manager() {
    return impl_->pump.router().focus_manager();
}

bool WindowHost::pump_once() {
    return impl_->pump_once(true);
}

bool WindowHost::pump_once(const runtime::RuntimeTick& tick, bool advance_pacer) {
    return impl_->pump_once(tick, advance_pacer);
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

// The lifecycle seam (ADR-050 section 1 rule 1, AF3). The driver forwards what it already folds;
// the caller owns the sink and must clear it before the sink dies, so a hand-written main that
// installs nothing keeps today's behaviour exactly.
void WindowHost::set_lifecycle_sink(LifecycleSink* sink) {
    impl_->lifecycle_sink = sink;
}

// Full-screen (docs/specs/window-chrome-v1.md section 13, issue #232). The platform owns the
// mechanism, the bounds to restore and the resolved state; the driver's part is the one diagnostic
// a platform without the feature owes the caller, and requesting the frame that shows the new size.
void WindowHost::set_fullscreen(bool fullscreen) {
    impl_->set_fullscreen(fullscreen);
}

bool WindowHost::fullscreen() const {
    return impl_->fullscreen();
}

bool WindowHost::fullscreen_supported() const {
    return impl_->fullscreen_supported();
}

void WindowHost::set_visible(bool visible) {
    impl_->set_visible(visible);
}

bool WindowHost::visible() const {
    return impl_->visible();
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

void WindowHost::Impl::apply_metrics(const hal::WindowMetrics& metrics) {
    if (!available) {
        return;
    }
    apply_metrics_lifecycle(metrics);
    // A hidden window can report a zero client area (a minimized Win32 window does), so a target
    // resize or a 0x0 relayout is skipped whenever the metrics say invisible - even when the pause
    // policy keeps rendering (pauseWhenOccluded=false). The scene tree stays intact
    // (docs/design/window-resize.md section 2 rule 6); the restore's metrics apply the real size.
    if (!metrics.occluded && !metrics.minimized) {
        if (swapchain != nullptr) {
            swapchain->resize({metrics.physical_width, metrics.physical_height});
        }
        host->resize(static_cast<float>(metrics.logical_width),
                     static_cast<float>(metrics.logical_height), metrics.dpi_scale);
    }
    host->set_window_metrics(metrics);
    apply_aa_policy(metrics);
    loop->mark_render_dirty();
}

// Full-screen (docs/specs/window-chrome-v1.md section 13, issue #232). The platform owns the
// mechanism, the pre-full-screen bounds and the window manager's answer; the driver's part is the
// one diagnostic a platform without the feature owes its caller. Nothing is cached here: a request
// that a window manager refuses simply leaves fullscreen() false, and the next frame's metrics tell
// the caller so.
void WindowHost::Impl::set_fullscreen(bool fullscreen) {
    if (!available || window == nullptr) {
        return;
    }
    if (!window->supports_fullscreen()) {
        if (!fullscreen_unsupported_reported) {
            fullscreen_unsupported_reported = true;
            std::fprintf(
                stderr,
                "TESSERA_WINDOW_UNSUPPORTED: full-screen is unavailable on this platform "
                "(no window manager with EWMH state support); the window keeps its windowed "
                "mode\n");
        }
        return;
    }
    window->set_fullscreen(fullscreen);
    // The mode change resizes the client area (the platform also reports a Resize), so request the
    // frame that lays out and paints at the new size. Asking for the repaint here is what makes a
    // programmatic toggle present promptly instead of waiting for the next dirty source.
    loop->mark_render_dirty();
    host->request_full_repaint();
}

bool WindowHost::Impl::fullscreen() const {
    return available && window != nullptr && window->metrics().fullscreen;
}

bool WindowHost::Impl::fullscreen_supported() const {
    return available && window != nullptr && window->supports_fullscreen();
}

// Showing or hiding the platform window (issue #181). Hiding is the app's own decision that the
// window is not on screen, so it is tracked separately from the platform's occlusion report: a
// `pauseWhenOccluded=false` config must not be able to make a hidden window paint.
void WindowHost::Impl::set_visible(bool visible) {
    if (!available || window == nullptr) {
        return;
    }
    if (visible) {
        window->show();
    } else {
        window->hide();
    }
    hidden_by_app_ = !visible;
    if (visible) {
        // Nothing painted while hidden describes the screen, so the first frame after showing is a
        // full one. `request_full_repaint` also marks the loop dirty, which is what wakes an
        // on-demand loop that has been parked since the hide.
        host->request_full_repaint();
    }
}

bool WindowHost::Impl::visible() const {
    return available && window != nullptr && !hidden_by_app_;
}

} // namespace tessera::app
