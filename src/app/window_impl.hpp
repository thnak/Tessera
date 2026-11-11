// SPDX-License-Identifier: MIT
//
// Private definition of WindowHost::Impl, shared by window.cpp (events, input, frame loop) and
// window_recovery.cpp (bounded device-loss recovery). Not a public header: ui/app code sees
// tessera/app/window.hpp.

#pragma once

#include "tessera/app/frame_guard.hpp"
#include "tessera/app/recovery_policy.hpp"
#include "tessera/app/window.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/rhi/swapchain.hpp"
#include "tessera/runtime/device_recovery.hpp"
#include "tessera/runtime/frame_pacer.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/runtime/steady_clock.hpp"
#include "tessera/ui/input_pump.hpp"
#include "window_internal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

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
    FrameResult render_and_present();
    void apply_metrics(const hal::WindowMetrics& metrics);
    WindowFrameStats run(std::uint32_t max_frames);

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
    std::unique_ptr<AppHost> host;

    ui::InputPump pump;
    WindowFrameStats stats;
    std::array<std::unique_ptr<rhi::CommandList>, detail::kFramesInFlight> in_flight;
    std::size_t slot = 0;
};

} // namespace tessera::app
