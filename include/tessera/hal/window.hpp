// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/caption.hpp"
#include "tessera/hal/geometry.hpp"
#include "tessera/hal/refresh_rate.hpp"
#include "tessera/hal/window_backdrop.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace tessera::ui {
class InputQueue;
}

namespace tessera::hal {

// Display cutouts (notch, punch-hole, camera island): insets say how much to avoid, the rectangles
// say where (docs/specs/os-events-v1.md section 2). `count == 0` is the answer of every platform
// that reports none; entries past `count` are zero. Build one with `make_cutout_info`.
inline constexpr std::size_t kMaxCutouts = 4;

struct CutoutInfo {
    std::array<WindowRect, kMaxCutouts> bounds{};
    std::uint8_t count = 0;
    // Curved-edge (waterfall) insets, zero when the display has none.
    WindowInsets waterfall_insets;
};

// Fold and hinge of a foldable display (docs/specs/os-events-v1.md section 2). The default is "no
// fold": every platform without a foldable reports it.
enum class FoldState : std::uint8_t { None, Flat, HalfOpened };
enum class FoldOrientation : std::uint8_t { None, Horizontal, Vertical };
enum class FoldOcclusion : std::uint8_t { None, Full };

struct FoldInfo {
    FoldState state = FoldState::None;
    FoldOrientation orientation = FoldOrientation::None;
    FoldOcclusion occlusion = FoldOcclusion::None;
    bool separating = false;
    // Hinge rectangle, logical pixels, window coordinates; zero when there is no fold.
    WindowRect hinge_bounds;
};

// Immutable-per-frame window/display snapshot. Styles, layout, and input see this type and never
// an OS handle, OS message, or swapchain.
struct WindowMetrics {
    std::uint32_t logical_width = 0;
    std::uint32_t logical_height = 0;
    std::uint32_t physical_width = 0;
    std::uint32_t physical_height = 0;
    float dpi_scale = 1.0f;
    WindowInsets insets;
    // Display cutouts; changes together with `insets` on `SafeAreaChanged`.
    CutoutInfo cutouts;
    // The on-screen keyboard (IME) and the system-gesture zones, as insets from each window edge;
    // zero when absent. `insets` stays the system bars. Changes on `ImeChanged`/`SafeAreaChanged`.
    WindowInsets ime_insets;
    WindowInsets gesture_insets;
    // The hinge, if any; changes on `FoldChanged`.
    FoldInfo fold;
    bool focused = true;
    bool occluded = false;
    bool minimized = false;
    // The material the OS actually draws behind a translucent window (glass-v1.md section 6). The
    // engine sees only this resolved value; no DWM, X11 or Wayland call crosses this boundary.
    BackdropMaterial backdrop = BackdropMaterial::NoMaterial;
    // True when the window is presented with a translucent client area. The swapchain's composite
    // alpha is the final gate on a backend that cannot present per-pixel alpha; the platform sets
    // this from its own window-system capability (DWM composition, an X11 compositing manager and
    // 32-bit visual, intrinsic on Wayland), and a false value here is the opaque fallback.
    bool transparent = false;
    // True when the requested backdrop or translucency could not be honored: the platform resolved
    // to the opaque fallback, and the driver reports TESSERA_GLASS_WINDOW_UNSUPPORTED once.
    bool backdrop_fallback = false;
    // True while the window is full-screen (window-chrome-v1.md section 13, issue #232). It is the
    // state the window manager resolved, not the last request: a platform that refuses the request
    // (no window manager, a refusing compositor) keeps reporting false. The bounds the window had
    // before entering are owned by the platform and restored on exit; the app reads this flag only.
    bool fullscreen = false;
    // Interactive client regions of the custom title bar, in logical pixels. Empty by default, which
    // is the system-chrome answer: every point is client area. The HAL resolves WM_NCHITTEST against
    // this list before app routing, so a scene node never sees a non-client hit-test result
    // (window-chrome-v1.md section 8.1, "Hit-test contract").
    std::array<CaptionRegion, kMaxCaptionRegions> caption_regions{};
    std::uint8_t caption_region_count = 0;
};

enum class WindowEventType {
    Resize,
    DpiChange,
    Focus,
    Blur,
    Occluded,
    Minimized,
    Restored,
    Close,
    // A system-wide user setting the app mirrors changed (Windows: animation effects toggled). The
    // event carries no value: the owner re-queries the setting it cares about, so no OS message or
    // parameter reaches UI code.
    SystemSettingsChanged,
    // The insets or the display cutouts changed (rotation, fold, bar visibility). The event's
    // metrics carry the new values; consecutive ones coalesce into the newest.
    SafeAreaChanged,
    // The on-screen keyboard moved or its inset changed; the metrics carry `ime_insets`.
    // Consecutive ones coalesce into the newest.
    ImeChanged,
    // The fold state or hinge rectangle changed; the metrics carry `fold`. Consecutive ones
    // coalesce into the newest.
    FoldChanged,
};

// Opaque native handles for renderer integration; UI/components must keep using WindowMetrics.
struct NativeWindowHandle {
    void* display = nullptr;
    void* window = nullptr;
};

// Opaque identity of the window's native host element for the platform accessibility root
// (Windows: the HWND that `UiaHostProviderFromHwnd` consumes). UI and accessibility code pass it by
// value and never inspect, cast, or dereference `token`: only a platform/UIA translation unit may
// interpret it, so no OS handle type leaks into a UI-layer type (ADR-020). A null token means the
// backend has no native host element (offscreen/null window or no accessibility platform).
struct AccessibilityHostHandle {
    void* token = nullptr;

    [[nodiscard]] bool valid() const noexcept {
        return token != nullptr;
    }
};

struct WindowEvent {
    WindowEventType type = WindowEventType::Resize;
    WindowMetrics metrics;
};

// Answers one WM_GETOBJECT (or the platform equivalent) for the window. `object_id` is the OS
// object id; `raw_provider` receives the opaque raw UI Automation root provider the platform hands
// to its OS call (UiaReturnRawElementProvider on Windows). The callback runs on the window's thread
// inside message dispatch and must never block or call UIA: it only reads a pointer another thread
// published. Returning false means "no root ready yet", and the platform falls back to its default
// handling (DefWindowProc, the MSAA default bridge) so a starting or failed accessibility bridge
// never stalls or breaks the window.
struct AccessibilityRoot {
    void* context = nullptr;
    bool (*answer)(void* context, std::intptr_t object_id, void*& raw_provider) = nullptr;
};

// Client-area size is logical pixels; the platform converts it to physical pixels with the
// per-monitor DPI scale. A zero width or height is invalid.
struct WindowDesc {
    std::string title = "Tessera";
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    bool visible = true;
    // The requested OS backdrop (the resolved root `window-backdrop`, glass-v1.md section 6). A
    // non-`Disabled` request implies a translucent client area; `transparent` also asks for one
    // without a named material. `win10_acrylic` opts into the undocumented Windows 10 accent policy
    // that only `win10Acrylic: true` may enable.
    WindowBackdrop backdrop = WindowBackdrop::Disabled;
    bool transparent = false;
    bool win10_acrylic = false;
};

// Platform window seam. Implementations live under src/platform/<os>/ and own every OS handle;
// this header stays OS-free so UI and rendering code can depend on it.
class Window {
public:
    virtual ~Window() = default;

    [[nodiscard]] virtual WindowMetrics metrics() const = 0;

    // The refresh rate of the display the window is on, in Hz, as the platform currently reports
    // it. `kUnknownRefreshHz` (0) is the documented "unknown" answer: no display, a platform or
    // server that cannot answer, a failed query, or a report outside the sane bounds of
    // tessera/hal/refresh_rate.hpp. It never guesses and never throws.
    //
    // It is not a per-frame query: the app window host reads it once, when the window is created,
    // to resolve WindowOptions::refresh_hz (an explicit option still wins). A monitor change is not
    // tracked in this slice; the OS-event display-capabilities item owns that. Must be called on
    // the thread that created the window. The default returns the unknown value, for platforms
    // that have no display-refresh source.
    [[nodiscard]] virtual double display_refresh_hz() const noexcept {
        return kUnknownRefreshHz;
    }

    // Copies pending events oldest first into `events` and returns the count written. Events that
    // do not fit stay queued for the next poll. Never blocks and never calls user code.
    virtual std::size_t poll_events(std::span<WindowEvent> events) = 0;

    // Idles for at most `timeout_seconds` while pumping the platform message queue, then returns
    // true if at least one WindowEvent is queued (or a Close is pending) and false on timeout.
    // Negative or non-finite timeouts are clamped to zero, so the call degrades to a poll. The
    // platform implementation only records normalized events; no user code runs from the wait.
    // Must be called on the thread that created the window. The default implementation is a
    // bounded idle sleep that reports no event, for platforms without an event source.
    virtual bool wait_for_events(double timeout_seconds);

    // Routes translated pointer, wheel, key and text input into `queue` as ui::InputEvent, in
    // logical pixels with the web sign for wheel deltas (see ui/key_code.hpp for key codes). The
    // queue is not owned: it must outlive the window or be reset with nullptr first. Events are
    // produced on the window's thread while it pumps messages, and a wait wakes while the queue
    // holds unconsumed input. Without a queue input is dropped; the default does nothing, for
    // platforms that do not translate input yet.
    virtual void set_input_queue(ui::InputQueue* queue) {
        (void)queue;
    }

    // Installs the UIA root handshake (see AccessibilityRoot). Must be called on the window's
    // thread; a null `answer` uninstalls it. Platforms without a UIA host ignore the hook. The
    // context must outlive the window or be uninstalled before it is destroyed.
    virtual void set_accessibility_root(AccessibilityRoot root) {
        (void)root;
    }

    virtual void show() = 0;
    virtual void hide() = 0;

    // Enters or leaves full-screen. The platform owns the mechanism and the bounds to restore: the
    // window's position and size before the call are remembered and reapplied on exit, so the app
    // never captures or restores a rectangle itself. The OS window shortcuts (F11, the window menu,
    // a compositor gesture) are not intercepted: they reach the platform and come back as a change
    // of `WindowMetrics::fullscreen`, and Escape is left to the app (media-components-v1.md
    // section 8). Must be called on the thread that created the window.
    //
    // It never blocks, throws, or fails: a platform that cannot honor the request (no backend, a
    // window manager that refuses the state change) leaves the window as it was, `metrics().fullscreen`
    // keeps telling the truth, and the caller reports one TESSERA_WINDOW_UNSUPPORTED diagnostic.
    // The default is a no-op, for platforms without full-screen support.
    virtual void set_fullscreen(bool fullscreen) {
        (void)fullscreen;
    }

    // False when full-screen cannot be requested at all on this platform, so a caller can report the
    // unsupported path once instead of calling set_fullscreen() and re-reading the metrics. True does
    // not promise that a request succeeds: a window manager may still refuse it, which the metrics
    // report. The default is false.
    [[nodiscard]] virtual bool supports_fullscreen() const noexcept {
        return false;
    }

    // Publishes the interactive regions of the framework's custom title bar (window-chrome-v1.md
    // section 8.1); an empty span returns the window to the system-chrome answer where every point is
    // client area. The regions are in logical pixels and are copied: the platform keeps them for its
    // non-client hit test, and the caller may pass a stack buffer. They also appear in
    // WindowMetrics::caption_regions, so a fake window and a real one report the same list. This is a
    // framework-only seam (WindowChrome); app code never calls it. The default stores nothing, for
    // platforms with no custom caption.
    virtual void set_caption_regions(std::span<const CaptionRegion> regions) {
        (void)regions;
    }

    // Asks the runtime for a frame; coalesced, so repeated calls before a frame are harmless.
    virtual void request_redraw() = 0;

    // Requests graceful shutdown. Implementations emit WindowEventType::Close; calling twice is
    // harmless.
    virtual void close() = 0;

    // Renderer/swapchain glue only. Win32 sets window (HWND); X11 sets display and window.
    [[nodiscard]] virtual NativeWindowHandle native_handle() const {
        return {};
    }

    // The window's native host element for the accessibility root. Only the platform/UIA layer may
    // interpret the opaque token; it is valid until the window is destroyed, and a null handle
    // means this backend cannot host a UI Automation root (offscreen/null window).
    [[nodiscard]] virtual AccessibilityHostHandle accessibility_host_handle() const {
        return {};
    }
};

// Creates a platform window from `desc`. Platforms without a window backend return nullptr;
// invalid descriptors (zero size) also return nullptr. The returned window must be destroyed on
// the thread that created it.
std::unique_ptr<Window> create_window(const WindowDesc& desc);

} // namespace tessera::hal
