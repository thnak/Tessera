// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/app/host.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/rendering/rhi/types.hpp"
#include "tessera/runtime/config.hpp"
#include "tessera/runtime/diagnostics.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tessera::rendering::rhi {
class Device;
}

namespace tessera::runtime {
class Clock;
}

namespace tessera::ui {
struct InputEvent;
}

namespace tessera::ui::text {
class TextMeasurer;
}

namespace tessera::app {

// Window creation and presentation options for WindowHost. `font_path` is required: the 2D sink
// shapes text through the loaded face (apps ship the font in their asset bundle; tools pass a
// file path).
struct WindowOptions {
    std::string title = "Tessera";
    std::uint32_t width = 900;
    std::uint32_t height = 620;
    bool visible = true;
    bool vsync = true;
    // Present-rate cap for the driver: 0 = uncapped (the display/vsync is the practical limit).
    // A non-zero cap makes the loop park on the pacer deadline instead of presenting early.
    // Negative or non-finite values are rejected by the WindowHost constructor.
    double max_fps = 0.0;
    // The display refresh, in Hz; 0 = unknown. A caller normally leaves this at 0: the host
    // resolves it once from the platform (`hal::Window::display_refresh_hz()`: the window's monitor
    // on Windows, the active RandR CRTC on X11/Xvfb, unknown elsewhere) and an explicit non-zero
    // value always wins over that query. It is the divisor base of the adaptive present cap
    // (docs/design/performance.md section 6, step 3) and the ceiling that cap may never exceed;
    // with 0 there is no divisor, so the adaptive cap stays at `max_fps` and the pacer keeps its
    // documented deadline pacing (runtime-config-v1.md section 5.2).
    double refresh_hz = 0.0;
    std::string font_path;
    std::shared_ptr<const ui::text::TextMeasurer> text_measurer;
    // Creates the backend device with the Khronos validation layer (and synchronization validation)
    // enabled. Off in production; GPU tests turn it on to prove a frame run is validation-clean.
    bool validation = false;
    // Borrowed time source for the host-owned caret blink (docs/specs/text-editing-v1.md section
    // 12). Null keeps the HAL monotonic clock; tests inject a ManualClock so blink phases are
    // deterministic. It is the caret tick clock only: frame dt and pacing stay on the system clock.
    runtime::Clock* clock = nullptr;
    // The project's `runtime` object (docs/specs/runtime-config-v1.md). Consumed in this slice:
    // `animationInterpolation` (pipeline paint interpolation), `pauseWhenOccluded`
    // (occluded/minimized pause) and the adaptive-quality keys `adaptiveQuality`, `powerPolicy`,
    // `renderScale`, `minRenderScale` and `frameBudgetMs` (the decision layer plus the
    // render-target consumer: a downgrade renders the scene into a smaller target and resamples it
    // up on present, while the window, swapchain, layout and glyph atlas keep their size).
    // Section 6.2 lists the keys still unwired. The defaults match RuntimeConfig{} exactly, so a
    // caller that does not set it keeps today's behaviour.
    tessera::RuntimeConfig runtime;
};

// Device health when run() returned. A caller distinguishes a clean exit (Ready, or Unavailable for
// a host that never had a presentation surface) from a run that stopped because a lost device could
// not be recovered (Lost; run() also sets WindowFrameStats::stalled).
enum class WindowDeviceState {
    Unavailable,
    Ready,
    Lost,
};

// Counters of one run() call (or the accumulation of pump_once() calls); a skipped run (no window
// backend or no presentation surface) reports available == false and zero frames.
struct WindowFrameStats {
    bool available = false;
    bool stalled = false;     // run() gave up after repeated frame failures (lost device, no image)
    std::uint32_t frames = 0; // frames painted and submitted
    std::uint32_t presented = 0;
    // Presented frames whose damage plan covered the whole target. The window path paints the whole
    // scene on every present, so this is the plan's own verdict: it proves a resume after a pause
    // repaints fully instead of trusting the pre-pause damage state.
    std::uint32_t full_repaints = 0;
    std::uint32_t idle_waits = 0;
    std::uint32_t input_events = 0;
    // Caret visibility flips produced by a blink tick (two per visible+hidden period). A flip
    // requests exactly one frame, so this also counts the caret's frames.
    std::uint32_t caret_blinks = 0;
    // Loop iterations that parked because the window was occluded/minimized. They acquire, paint
    // and present nothing and are not counted as idle_waits (different reason, same no-work rule).
    std::uint32_t paused_iterations = 0;
    // Bounded device-loss recovery (docs/design/window-resize.md section 7): successful recoveries
    // in this run, attempts charged against the budget (app/recovery_policy.hpp), and the final
    // health.
    std::uint32_t recovered = 0;
    std::uint32_t recovery_attempts = 0;
    WindowDeviceState device_state = WindowDeviceState::Unavailable;
    // Swapchain frame pacing (docs/design/gpu/frames.md section 5.1), sampled once per frame
    // attempt from a host counter query: the completion value the GPU reached, how many frame slots
    // are still in flight (0..2), and how many acquires skipped the tick because no slot was
    // reusable. All zero on backends without timeline pacing.
    std::uint64_t timeline_value = 0;
    std::uint32_t frames_in_flight = 0;
    std::uint32_t slots_skipped = 0;
    // Adaptive render scale (docs/design/performance.md section 6.1): the controller's multiplier
    // in force for the last frame, the extent the scene actually rendered into, and the swapchain
    // extent it was presented at. At the default `renderScale` of 1.0 the two extents are equal,
    // `scaled_frames` stays 0 and the frame is the unchanged full-quality direct path.
    float render_scale = 1.0f;
    rendering::rhi::Extent2D target_extent{};
    rendering::rhi::Extent2D present_extent{};
    // Frames presented through a resample of the smaller target (a downgrade was in force), and
    // frames that stayed at full quality because the backend refused the extra target.
    std::uint32_t scaled_frames = 0;
    std::uint32_t scale_fallbacks = 0;
    // Adaptive present cap (docs/design/performance.md section 6, step 3): the cap the pacer is
    // currently applying (0 = uncapped), the present interval that cap derives (0 = present when
    // ready), and how many times a quality change moved the cap. The cap is applied through the
    // pacer's existing rate-change path, so a downgrade takes effect at the next frame boundary
    // without a blocking wait or a swapchain recreation.
    double present_cap_hz = 0.0;
    double present_interval_seconds = 0.0;
    std::uint32_t present_cap_changes = 0;
    // GPU frame-time signal (docs/design/performance.md section 6.1): which source produced the
    // last adaptive-quality observation, how many observations used a completed submission's GPU
    // interval, and how many fell back to the CPU time even though the device reported usable
    // timestamps (the first frame after startup, a skipped read, a missing interval). A fallback is
    // counted, never reported as a GPU number.
    runtime::FrameTimeSource frame_time_source = runtime::FrameTimeSource::Cpu;
    std::uint32_t gpu_frame_time_samples = 0;
    std::uint32_t gpu_frame_time_fallbacks = 0;
};

// The production window driver: owns the platform window, the Vulkan device + swapchain, the
// Batched2D renderer and the font/text stack, and drives an AppHost frame loop:
//   events -> InputPump -> update scheduler -> style/layout/sync/damage -> paint -> present.
// Resize/DPI changes resize the swapchain and the host; a close event ends the loop.
//
// A static scene idles: the loop parks on the bounded platform wait for the pacer's next deadline
// (clamped by app/idle_policy.hpp) and presents nothing until input, a resize or a scheduled update
// asks for a frame that changed. `run(max_frames > 0)` forces that many frames (CI smoke) even when
// nothing is dirty. The loop never waits on the GPU; teardown drains the submitted lists with a
// bounded per-list poll, and a host can run() again (the input queue is reattached on entry).
//
// A window the platform reports as occluded or minimized is paused (docs/design/window-resize.md
// section 8): no frame is begun, so nothing is acquired, painted or presented and the animation
// runtime does not tick, while the loop keeps pumping events/input and parking on the bounded wait.
// Focus/blur alone never pauses: a visible background window keeps painting. The pause is cleared
// by the platform's restore/visible event or by a `WindowMetrics` snapshot whose `occluded` and
// `minimized` are both false; the resume edge requests a full repaint, so the first frame after a
// pause covers the whole target. A paused window makes `run(max_frames)` wait for the window to
// become visible again (or to close): the pause wins over the forced-frame path.
//
// A lost device does not end the run: the driver releases the lost generation, lets the runtime's
// DeviceRecoveryController attempt recovery, rebuilds the device when that is spent, and recreates
// the swapchain, renderer, glyph atlas and sink for the current window metrics before repainting a
// full frame. The attempt budget is app/recovery_policy.hpp; when it is spent (or the presentation
// stack cannot be rebuilt) run() returns with stats.stalled and stats.device_state == Lost.
//
// The host owns the window's caret blink (docs/specs/text-editing-v1.md section 12): the app
// expresses focus/editability/selection through set_caret_active(), a painter reads
// caret_visible(), and each iteration of run()/pump_once() ticks the blink against the configured
// clock. A flip requests exactly one frame; while the caret is inactive, or while the window is
// paused, nothing ticks and the loop stays idle with no presents.
//
// One window per host (docs/design/app-host.md section 5). The host is single-threaded and must be
// created, run and destroyed on the same thread as the window.
class WindowHost final {
public:
    // `root` and `scheduler` are borrowed; `sheet` is owned. The root's inline styles for window
    // size are host-owned; an app may set additional root inline styles before run().
    WindowHost(ui::Node& root, ui::style::CompiledStyleSheet sheet, WindowOptions options,
               ui::UpdateScheduler* scheduler = nullptr);
    ~WindowHost();

    WindowHost(const WindowHost&) = delete;
    WindowHost& operator=(const WindowHost&) = delete;
    WindowHost(WindowHost&&) = delete;
    WindowHost& operator=(WindowHost&&) = delete;

    // False when the platform has no window backend or the device cannot present; run() then
    // returns immediately with stats().available == false.
    [[nodiscard]] bool available() const;

    // Runs until the window closes. With max_frames > 0 it stops after that many presented frames,
    // forcing a frame even when the scene is clean (the bounded CI path). Callable more than once.
    WindowFrameStats run(std::uint32_t max_frames = 0);

    // One bounded driver iteration for automation and tests: pump the platform event queue and
    // queued input, honour the occlusion/minimize pause, tick the loop and paint/present at most
    // one frame. Returns true when that iteration presented a frame. It never parks, never retries
    // and never services a lost device (run() owns pacing, retry and recovery), so a caller decides
    // when to call it again; it is not a loop of its own and must not be called in a tight spin.
    //
    // It shares state with run(): the input queue is reattached on entry, the first call paints
    // (startup frames are a frame cause), device_state starts Ready on a healthy host, and every
    // counter accumulates in stats() exactly as run() does. Paused iterations count in
    // paused_iterations and return false.
    bool pump_once();

    // Queues one normalized input event as if the platform had produced it; the next run() pumps
    // and dispatches it through the same InputPump/InputRouter path as real OS input. Used by
    // automation and tests that drive a window without a mouse/keyboard.
    bool post_input(const ui::InputEvent& event);

    // Applies a metrics snapshot as if the platform had reported a resize/DPI change: the swapchain
    // is resized, the AppHost re-lays out at the new logical size and the next frame is requested.
    // Tests use it to exercise the resize path without an OS-driven window resize.
    void apply_metrics(const hal::WindowMetrics& metrics);

    // The app's one focus/editability/selection input for the caret: pass `focused && !read_only &&
    // !has_selection` (EditController::caret_blink_active). Deactivating hides the caret and stops
    // ticking, so a focused-out window requests no frames at all; activating shows it immediately
    // and restarts the cycle at the clock's current time, requesting one frame for the showing.
    void set_caret_active(bool active);

    // What a painter reads: the caret's current visibility (false while inactive). Paint itself
    // stays timer-free; the driver is the only owner of the blink state.
    [[nodiscard]] bool caret_visible() const;

    [[nodiscard]] const WindowFrameStats& stats() const;
    // The host the driver feeds; expose for tests and late root style changes. Reference is stable
    // for the WindowHost lifetime.
    [[nodiscard]] AppHost& host();

#if defined(TESSERA_VULKAN_TEST_HOOKS)
    // Test seam, compiled only when the backend's fault-injection hooks are on: the live RHI
    // device, so a GPU test can inject one device-lost fault through the backend's existing seam
    // (VulkanDevice::inject_fault_for_testing). Not part of the public contract; the type stays
    // opaque to app code.
    [[nodiscard]] rendering::rhi::Device& device_for_testing();
#endif

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct CaptureOptions {
    std::uint32_t width = 380;
    std::uint32_t height = 260;
    float dpi_scale = 1.0f;
    std::string font_path;
    std::shared_ptr<const ui::text::TextMeasurer> text_measurer;
    // Optional update scheduler (generated components flush their state changes at its flush).
    ui::UpdateScheduler* scheduler = nullptr;
};

struct CapturedFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Tightly packed RGBA8, one row after another, origin at the top-left.
    std::vector<std::uint8_t> rgba;
};

// Windowless one-frame capture through the same AppHost + Batched2DSink + Vulkan path the window
// uses: offscreen target, full paint, readback. `prepare` runs after the first layout frame (so
// bounds and hit testing are valid) and before the captured frame; tools use it to click. Returns
// nullopt when the machine has no Vulkan device (a documented skip, never an error).
std::optional<CapturedFrame> capture_frame(ui::Node& root, ui::style::CompiledStyleSheet sheet,
                                           const CaptureOptions& options,
                                           const std::function<void()>& prepare = {});

} // namespace tessera::app
