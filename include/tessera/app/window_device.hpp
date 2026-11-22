// SPDX-License-Identifier: MIT
//
// `WindowDevice`: the one GPU device, cache set and runtime loop an app's windows share (issue
// #228, ADR-050 answer 3). See docs/design/multi-window.md for the ownership table and the
// reasoning.
//
// **Why this type exists.** Before it, `WindowHost` created its own `rhi::Device`, `RuntimeLoop`,
// `FramePacer` and a full copy of every per-device cache. Two windows meant two devices, two glyph
// atlases, two pipeline sets and two recovery controllers, and `run()` was a blocking loop per
// window, so a second window could not exist at all.
//
// **The rule that decides what lives here.** If an object is created against the *device* or the
// *swapchain format*, it is shared and lives here. If it is created against a *surface* or a
// *scene*, it is per window and stays in `WindowHost`. That is why the renderer, the text stack,
// the pipelines and the white texture are here while the swapchain, the `AppHost`, the input pump
// and the accessibility host are not.
//
// **The one number this changes.** `Batched2DRenderer` keeps a `frame_index_` that it increments on
// every `begin()` and uses to index its vertex ring and glass target pool. With one renderer
// serving N windows, one tick can call `begin()` N times, so the ring must hold `frames_in_flight *
// window_count` frames. `renderer_frames_in_flight()` is that number, and it is the only place the
// refactor changes a value rather than moving an object.
//
// **Threading.** Everything here runs on the UI thread, exactly like the driver it serves. Nothing
// waits, sleeps, joins or detaches (ADR-021).

#pragma once

#include "tessera/hal/window.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/runtime/config.hpp"
#include "tessera/runtime/device_recovery.hpp"
#include "tessera/runtime/frame_pacer.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/runtime/steady_clock.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace tessera::app {

namespace rhi = tessera::rendering::rhi;

// Forward-declared: `WindowOptions` lives in window.hpp, which includes this header. The two
// functions that take it are defined in the .cpp, so the incomplete type is enough here.
struct WindowOptions;
struct WindowDeviceOptions;
// The windows this device drives (issue #180). `WindowHost` is declared in window.hpp, which
// includes this header, so the list holds pointers to the incomplete type and only the .cpp that
// drives them needs the definition.
class WindowHost;

namespace detail {
class TextStack;
} // namespace detail

// What a shared `WindowDevice` is created with (issue #228, docs/design/multi-window.md). It is the
// subset of `WindowOptions` that describes the *device* rather than a window: the validation layer,
// the presentation family, and the font the shared text stack loads. Everything else - title, size,
// vsync, the runtime config, the scene - is per window and stays in `WindowOptions`.
//
// A single-window app never names this type: the `WindowHost` constructor that takes only
// `WindowOptions` builds a private one-window group from the matching fields, which is exactly the
// pre-refactor behaviour.
struct WindowDeviceOptions {
    // Creates the backend device with the Khronos validation layer (and synchronization validation)
    // enabled. Off in production; GPU tests turn it on to prove a frame run is validation-clean.
    bool validation = false;
    // The presentation family the device builds surfaces for. `Auto` keeps the environment-based
    // resolution, which is what a caller with no window yet wants.
    rhi::SurfacePlatform surface = rhi::SurfacePlatform::Auto;
    // The font the shared text stack loads. Empty means "the window's own `font_path`", which is
    // what a one-window group passes; a multi-window group names it once here so every window
    // measures and paints with the same face.
    std::string font_path;
    // The RHI backend. `Vulkan` is production; `Null` is the deterministic backend the multi-window
    // acceptance runs on, because it records commands instead of presenting and therefore needs no
    // surface. A null-backend group still creates a real platform window (the HAL is independent of
    // the RHI), so the window lifecycle, input and metrics paths are exercised for real.
    rhi::Backend backend = rhi::Backend::Vulkan;
};

// How many windows one `WindowDevice` may serve. It bounds the renderer's frame ring
// (`kFramesInFlight * max_windows`) and the recovery fan-out, so it is a real capacity rather than
// a hint. Four covers the desktop cases V1 names (a main window plus a detached panel, a dialog and
// a preview) without sizing the ring for a number nobody uses.
inline constexpr std::size_t kMaxWindowsPerDevice = 4;

// Two frames in flight per window: the render path never waits on the GPU, it skips a slot that is
// still busy and retries on the next tick (docs/design/window-resize.md section 0).
inline constexpr std::size_t kFramesInFlightPerWindow = 2;

// The shared device, its per-device caches and the app's one runtime loop.
//
// Construction creates the device and the loop; the caches are created lazily by
// `ensure_presentation(format)`, because a pipeline is created against a swapchain format and no
// window exists yet at construction time. The first window to present establishes the format; every
// later window in the group must present through the same format, which is what makes one pipeline
// set correct for all of them.
class WindowDevice final {
public:
    // `validation` turns the Khronos validation layer on for this device (a GPU test reads the
    // backend's validation_message_count() to assert a run was clean). `surface` is the
    // presentation family the device builds surfaces for; `Auto` keeps the environment-based
    // resolution.
    explicit WindowDevice(const WindowDeviceOptions& options = {});
    ~WindowDevice();

    WindowDevice(const WindowDevice&) = delete;
    WindowDevice& operator=(const WindowDevice&) = delete;
    WindowDevice(WindowDevice&&) = delete;
    WindowDevice& operator=(WindowDevice&&) = delete;

    // False when the machine has no usable device. A host created against an unavailable group
    // reports `available() == false` and presents nothing, exactly as a single-window host did.
    [[nodiscard]] bool available() const noexcept {
        return device_ != nullptr;
    }

    [[nodiscard]] rhi::Device& device() {
        return *device_;
    }
    [[nodiscard]] rhi::Device* device_or_null() noexcept {
        return device_.get();
    }
    [[nodiscard]] runtime::RuntimeLoop& loop() noexcept {
        return *loop_;
    }
    [[nodiscard]] runtime::FramePacer& pacer() noexcept {
        return pacer_;
    }
    [[nodiscard]] runtime::DeviceRecoveryController* recovery() noexcept {
        return recovery_.get();
    }

    // The renderer's frame ring depth for this group: `kFramesInFlightPerWindow * max_windows`.
    [[nodiscard]] static constexpr std::uint32_t renderer_frames_in_flight() noexcept {
        return static_cast<std::uint32_t>(kFramesInFlightPerWindow * kMaxWindowsPerDevice);
    }

    // Creates the per-device caches for `format` if they do not exist yet, and returns false when
    // the backend refuses any of them. Idempotent for the same format; a different format is a
    // contract violation (two windows in one group must present through one format) and returns
    // false rather than silently rebuilding the pipelines under the first window.
    [[nodiscard]] bool ensure_presentation(rhi::TextureFormat format, const WindowOptions& options);

    // Releases every per-device cache. Called before the device is replaced on a loss, and at
    // teardown. The swapchains belong to the windows and are released by them.
    void release_presentation();

    // Replaces the device object after a loss whose in-place `recover()` is spent. The caches are
    // released first; every window then recreates its own presentation against the new device.
    [[nodiscard]] bool replace_device(const WindowDeviceOptions& options);

    // --- the shared caches, valid between ensure_presentation and release_presentation ---------

    [[nodiscard]] rendering::gpu::Batched2DRenderer* renderer() noexcept {
        return renderer_.get();
    }
    [[nodiscard]] detail::TextStack* text() noexcept {
        return text_.get();
    }
    [[nodiscard]] ui::paint::Batched2DSink* sink() noexcept {
        return sink_.get();
    }
    [[nodiscard]] rhi::Pipeline* pipeline() noexcept {
        return pipeline_.get();
    }
    [[nodiscard]] rhi::Pipeline* present_pipeline() noexcept {
        return present_pipeline_.get();
    }
    [[nodiscard]] rhi::Pipeline* lcd_pipeline() noexcept {
        return lcd_pipeline_.get();
    }
    [[nodiscard]] rhi::Pipeline* glass_blur_pipeline() noexcept {
        return glass_blur_pipeline_.get();
    }
    [[nodiscard]] rhi::Pipeline* glass_composite_pipeline() noexcept {
        return glass_composite_pipeline_.get();
    }
    [[nodiscard]] rhi::Texture* white() noexcept {
        return white_.get();
    }
    [[nodiscard]] rhi::TextureFormat format() const noexcept {
        return format_;
    }
    [[nodiscard]] bool has_presentation() const noexcept {
        return renderer_ != nullptr;
    }

    // The sample count the shared presentation stack was built with (1 or 4). Every window in the
    // group presents through the same pipelines, so they must all agree on it.
    [[nodiscard]] std::uint32_t presentation_sample_count() const noexcept {
        return presentation_sample_count_;
    }
    void set_presentation_sample_count(std::uint32_t count) noexcept {
        presentation_sample_count_ = count;
    }

    // How many windows are currently attached. A host attaches in its constructor and detaches in
    // its destructor; the count is what a teardown uses to decide whether the caches are still
    // needed.
    [[nodiscard]] std::size_t window_count() const noexcept {
        return window_count_;
    }
    void attach_window() noexcept {
        ++window_count_;
    }
    void detach_window() noexcept {
        if (window_count_ > 0) {
            --window_count_;
        }
    }

    // --- the window list (issue #180) ---------------------------------------------------------
    // The windows this device drives, in attach order. A host registers itself in its constructor
    // and unregisters in its destructor, so the list is exactly the live set.
    //
    // The list is what makes one loop drive N windows: `pump_once()` walks it and gives each window
    // one bounded iteration, so a caller that owns several windows does not have to interleave them
    // by hand (and cannot forget one). It is a fixed array, so registration never allocates.
    //
    // A window that is not `available()` (no platform window, no presentation) is skipped by the
    // walk rather than reported as a failure: a group may hold a window that failed to build while
    // its siblings present.
    bool register_window(WindowHost& window) noexcept;
    bool unregister_window(WindowHost& window) noexcept;
    [[nodiscard]] std::span<WindowHost* const> windows() const noexcept {
        return std::span<WindowHost* const>(windows_.get(), window_list_size_);
    }
    [[nodiscard]] std::size_t window_list_size() const noexcept {
        return window_list_size_;
    }

    // One bounded iteration of every registered window: each one pumps its events and input,
    // honours its own pause, and paints at most one frame. Returns how many windows presented.
    //
    // This is the multi-window form of `WindowHost::pump_once()`: the caller decides when the next
    // iteration happens, and the loop never parks, retries or services a lost device (that stays
    // `WindowHost::run()`'s job). A window whose iteration presented nothing does not stop its
    // siblings - which is the whole point of driving them from one place.
    std::size_t pump_windows_once();

private:
    std::unique_ptr<rhi::Device> device_;
    std::unique_ptr<runtime::RuntimeLoop> loop_;
    runtime::FramePacer pacer_;
    std::unique_ptr<runtime::DeviceRecoveryController> recovery_;
    runtime::SteadyClock clock_;

    // The shared caches. Declaration order is dependency order: the sink borrows the renderer and
    // the text renderer, and the renderer borrows the pipeline and the white texture, so they are
    // destroyed in the reverse order `release_presentation` spells out.
    std::unique_ptr<rhi::Pipeline> pipeline_;
    std::unique_ptr<rhi::Pipeline> present_pipeline_;
    std::unique_ptr<rhi::Pipeline> lcd_pipeline_;
    std::unique_ptr<rhi::Pipeline> glass_blur_pipeline_;
    std::unique_ptr<rhi::Pipeline> glass_composite_pipeline_;
    std::unique_ptr<rhi::Texture> white_;
    std::unique_ptr<rendering::gpu::Batched2DRenderer> renderer_;
    std::unique_ptr<detail::TextStack> text_;
    std::unique_ptr<ui::paint::Batched2DSink> sink_;

    // `TextureFormat` has no `Undefined` enumerator, so "no format yet" is a separate flag rather
    // than a sentinel value.
    rhi::TextureFormat format_ = rhi::TextureFormat::R8G8B8A8Unorm;
    bool has_format_ = false;
    std::uint32_t presentation_sample_count_ = 1;
    std::size_t window_count_ = 0;
    // The live window list (issue #180). Sized by `kMaxWindowsPerDevice`, the same bound the
    // renderer's frame ring uses, so a registration can never exceed what the ring can serve.
    std::unique_ptr<WindowHost*[]> windows_ = std::make_unique<WindowHost*[]>(kMaxWindowsPerDevice);
    std::size_t window_list_size_ = 0;
};

} // namespace tessera::app
