// SPDX-License-Identifier: MIT
//
// Private definition of WindowHost::Impl, shared by window.cpp (events, input, frame loop) and
// window_recovery.cpp (bounded device-loss recovery). Not a public header: ui/app code sees
// tessera/app/window.hpp.

#pragma once

#include "tessera/app/frame_guard.hpp"
#include "tessera/app/recovery_policy.hpp"
#include "tessera/app/system_motion.hpp"
#include "tessera/app/window.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/rhi/swapchain.hpp"
#include "tessera/rendering/rhi/timestamp.hpp"
#include "tessera/runtime/device_recovery.hpp"
#include "tessera/runtime/frame_pacer.hpp"
#include "tessera/runtime/frame_time_source.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/runtime/steady_clock.hpp"
#include "tessera/ui/input_pump.hpp"
#include "tessera/ui/text/caret_blink.hpp"
#include "window_config.hpp"
#include "window_internal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace tessera::app {

namespace rhi = tessera::rendering::rhi;

namespace detail {

// Two frames in flight: the render path never waits on the GPU, it skips a slot that is still busy
// and retries on the next tick (docs/design/window-resize.md section 0).
inline constexpr std::size_t kFramesInFlight = 2;
inline constexpr std::size_t kMaxWindowEvents = 16;

// Teardown drain of the lists this loop submitted: a hard budget, so a wedged or lost device cannot
// stall shutdown, and the RHI owns the command resources until their fence signals.
inline constexpr double kDrainBudgetSeconds = 0.25;
inline constexpr std::uint64_t kPollSliceMs = 1;

// Brief park between recovery attempts that are still waiting on the CPU side (an in-place recovery
// that has not reported yet). It is a retry yield on the platform event wait, never a spin.
inline constexpr double kRecoveryDrainBudgetSeconds = 0.05;

} // namespace detail

struct WindowHost::Impl {
    Impl(ui::Node& root, ui::style::CompiledStyleSheet sheet, WindowOptions options,
         ui::UpdateScheduler* update_scheduler);
    ~Impl();

    // Outcome of one frame attempt; a lost device is state the run services, not a painted frame.
    enum class FrameResult { Presented, Skipped, DeviceLost };

    void idle(double seconds);
    // Polls the lists this loop submitted under a hard budget (never a device-wide wait). A lost
    // device never completes its fences, so its generation is closed out without polling.
    void release_in_flight(double budget_seconds);
    void recreate_swapchain();
    FrameResult render_and_present(bool full_repaint);
    // Copies the swapchain's frame-pacing counters into stats(). A host counter query, never a
    // wait, so it is cheap enough to sample around each frame attempt.
    void sample_frame_sync();
    void apply_metrics(const hal::WindowMetrics& metrics);
    WindowFrameStats run(std::uint32_t max_frames);

    // --- lifecycle, pause and the single-iteration seam (window_lifecycle.cpp) ---------------
    // True while the platform reports the window as occluded/minimized: no frame may be acquired,
    // painted or presented, and the frame pipeline/animation runtime must not tick.
    [[nodiscard]] bool paused() const noexcept {
        return paused_;
    }
    // Folds one normalized platform event into the driver state. True means the window asked to
    // close (the caller ends the run).
    bool handle_window_event(const hal::WindowEvent& event);
    // Drains queued input through the pump and marks the render dirty when the scene went dirty.
    void pump_input();
    // Recomputes `paused_` from the lifecycle flags and the latest metrics; a pause -> resume edge
    // latches a full repaint so nothing on screen can be stale.
    void update_pause_state();
    // One bounded iteration shared by pump_once(): events -> input -> pause -> tick -> one frame.
    bool pump_once();

    // --- adaptive present cap (docs/design/performance.md section 6, step 3) ------------------
    // Applies the controller's decided present cap to the pacer through the pacer's existing
    // rate-change path (FramePacer::update), which is the same mechanism a scripted refresh change
    // uses: the new interval takes effect at the next frame boundary, with no blocking wait and no
    // swapchain recreation. Called after each frame observation; a cap that did not move is a no-op
    // so the running deadline is not resynced for nothing.
    void apply_present_cap(double now_seconds);

    // --- adaptive render scale (docs/design/performance.md section 6.1) ----------------------
    // Ensures the offscreen target `slot` renders into when the controller's renderScale is below
    // full quality is the requested extent. One target per frame slot, because a target may still
    // be read by the frame slot's in-flight blit: reusing a single image across two frames in
    // flight would be a cross-submission read/write hazard. Returns false when the backend refuses,
    // in which case the frame stays on the direct full-size path rather than dropping. Called at a
    // frame boundary and only when the decided extent changed, so a downgrade allocates one image.
    bool ensure_scaled_target(std::size_t frame_slot, rhi::Extent2D extent);

    // --- GPU frame-time signal (docs/design/performance.md section 6.1) ----------------------
    // The device probe's answer, resolved once per presentation generation. False (null backend,
    // no timestamp queries, a bad period) keeps the CPU frame time for every observation.
    bool gpu_timestamps_usable = false;
    // The GPU interval of a submission whose slot completed, waiting to be fed to the one
    // observation of a presented frame. It is at most two frames old because a timestamp value is
    // only readable after its submission completed; a frame that observes before a new interval
    // exists falls back to the CPU time and counts the fallback.
    std::optional<double> pending_gpu_frame_time_ms;
    // Takes the pending GPU interval (and clears it). The one place both driver paths consume it.
    std::optional<double> take_gpu_frame_time_ms();
    // Selects the frame-time source, records it in stats, and feeds the host. `cpu_frame_time_ms`
    // is the frame's CPU cost through present, which stays the fallback.
    runtime::FrameTimeSample observe_frame_time(double cpu_frame_time_ms);

    // --- host-owned caret blink (window_lifecycle.cpp) --------------------------------------
    // The clock the caret ticks against: the borrowed WindowOptions::clock when set (tests inject a
    // ManualClock), else the HAL monotonic clock. Rendering dt/pacing never use this.
    [[nodiscard]] double caret_now_seconds() const {
        const runtime::Clock* source = options.clock;
        return source != nullptr ? source->now_seconds() : runtime::SteadyClock{}.now_seconds();
    }
    // One iteration's caret tick: called after the pause guard and before the frame decision of
    // both pump_once() and run(). Inactive (unfocused) blinks are not ticked at all, so they
    // request nothing and the loop stays idle. A visibility flip requests exactly one frame.
    void tick_caret();
    // Focus/editability/selection policy input; false stops ticking and requests nothing.
    void set_caret_active(bool active);

    // --- device-loss recovery (window_recovery.cpp) -----------------------------------------
    // Creates the swapchain, 2D pipeline, renderer, text stack and sink on `device` at the window's
    // current metrics; false (never a throw) when the backend refuses the request.
    bool create_presentation();
    // Releases every presentation object of the current device generation. It never waits
    // device-wide: the backend's own teardown owns that wait and its generation guards make this
    // safe after a loss.
    void release_presentation();
    // release_presentation() then create_presentation(): nothing of the old generation is reused.
    bool recreate_presentation();
    // Replaces the backend device object, because a loss's in-place recover() is one-shot. The
    // platform window (and its surface handle) stays valid; the host recreates against it.
    bool replace_device();
    // Services one blocked/failed/recovered runtime tick. False means the run must stop.
    bool service_device_loss(const runtime::RuntimeTick& tick, RecoveryPolicy& policy,
                             FrameFailureGuard& failures);
    bool service_recovery(RecoveryPolicy& policy, FrameFailureGuard& failures);

    ui::Node& root;
    WindowOptions options;
    ui::UpdateScheduler* scheduler = nullptr;
    bool available = false;

    std::unique_ptr<hal::Window> window;
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<rhi::Swapchain> swapchain;
    // Adaptive render-scale targets (docs/design/performance.md section 6.1): allocated only while
    // the controller reports a scale below full quality, at the decided extent, and never at the
    // window's physical size. One per frame slot, like the renderer's vertex-buffer slots: the slot
    // gate (see kFramesInFlight) is what proves the previous read of that image completed.
    // `scaled_present_supported` is the swapchain generation's fixed answer to whether a smaller
    // target can be resampled into its images.
    std::array<std::unique_ptr<rhi::RenderTarget>, detail::kFramesInFlight> scaled_targets;
    std::array<rhi::Extent2D, detail::kFramesInFlight> scaled_target_extents{};
    bool scaled_present_supported = false;
    // True when the platform window presents a translucent client area (glass-v1.md section 6), so
    // every frame clears to transparent black instead of the app window background.
    bool translucent_presentation = false;
    // True once the per-pixel-alpha fallback (glass-v1.md section 6) was reported, so a swapchain
    // recreation during a run does not repeat TESSERA_GLASS_WINDOW_UNSUPPORTED.
    bool alpha_fallback_reported = false;
    std::unique_ptr<rhi::Pipeline> pipeline;
    std::unique_ptr<rhi::Texture> white;
    std::unique_ptr<rendering::gpu::Batched2DRenderer> renderer;
    std::unique_ptr<detail::TextStack> text;
    std::unique_ptr<ui::paint::Batched2DSink> sink;
    // D7: polled once per RuntimeLoop::tick(); it owns the backend's single bounded in-place
    // recovery attempt. Replaced together with the device.
    std::unique_ptr<runtime::DeviceRecoveryController> recovery;
    runtime::SteadyClock clock;
    std::unique_ptr<runtime::RuntimeLoop> loop = std::make_unique<runtime::RuntimeLoop>(clock);
    // Present-rate cap and deadline source for the idle paths (0 = uncapped).
    runtime::FramePacer pacer;
    // The cap the pacer is currently applying (0 = uncapped). Starts at the configured `maxFps`,
    // which is what the pacer was constructed with, so the first observation only calls update()
    // when the controller actually moved the cap.
    double applied_present_cap_hz = 0.0;
    std::unique_ptr<AppHost> host;
    // Last OS reduced-motion value pushed to `host`; a SystemSettingsChanged event re-queries it.
    MotionPreference motion;

    ui::InputPump pump;
    WindowFrameStats stats;
    // Which lifecycle states pause, mapped once from WindowOptions::runtime (window_config.hpp).
    // Focus/blur is not in this policy, so it can never contribute.
    WindowPausePolicy pause_policy;
    // The window's one caret blink (500/500 ms defaults), host-owned so every driver path - run(),
    // pump_once(), pause - sees the same state. Starts inactive: no field owns focus until the app
    // calls set_caret_active(true), so a fresh window requests nothing for the caret.
    ui::text::CaretBlink caret;
    std::array<std::unique_ptr<rhi::CommandList>, detail::kFramesInFlight> in_flight;
    std::size_t slot = 0;

    // Pause state: `lifecycle_paused_` comes from the platform's Minimized/Occluded events (cleared
    // by Restored), `metrics_paused_` from the latest hal::WindowMetrics (occluded || minimized),
    // and `paused_` is their union. Focus/blur never contributes (a visible background window
    // keeps painting). A true->false edge is the resume that requests a full repaint.
    bool lifecycle_paused_ = false;
    bool metrics_paused_ = false;
    bool paused_ = false;
    // Last frame time shared by run() and pump_once(), so a resume gets a normal dt instead of the
    // whole pause duration (and paused iterations refresh it).
    double last_tick_seconds_ = 0.0;
};

} // namespace tessera::app
