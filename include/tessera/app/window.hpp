// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/app/host.hpp"
#include "tessera/hal/window.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
    std::string font_path;
    std::shared_ptr<const ui::text::TextMeasurer> text_measurer;
};

// Counters of one run() call; a skipped run (no window backend or no presentation surface) reports
// available == false and zero frames.
struct WindowFrameStats {
    bool available = false;
    bool stalled = false;     // run() gave up after repeated frame failures (lost device, no image)
    std::uint32_t frames = 0; // frames painted and submitted
    std::uint32_t presented = 0;
    std::uint32_t idle_waits = 0;
    std::uint32_t input_events = 0;
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

    // Queues one normalized input event as if the platform had produced it; the next run() pumps
    // and dispatches it through the same InputPump/InputRouter path as real OS input. Used by
    // automation and tests that drive a window without a mouse/keyboard.
    bool post_input(const ui::InputEvent& event);

    // Applies a metrics snapshot as if the platform had reported a resize/DPI change: the swapchain
    // is resized, the AppHost re-lays out at the new logical size and the next frame is requested.
    // Tests use it to exercise the resize path without an OS-driven window resize.
    void apply_metrics(const hal::WindowMetrics& metrics);

    [[nodiscard]] const WindowFrameStats& stats() const;
    // The host the driver feeds; expose for tests and late root style changes. Reference is stable
    // for the WindowHost lifetime.
    [[nodiscard]] AppHost& host();

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
