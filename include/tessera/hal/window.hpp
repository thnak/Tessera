// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/appearance.hpp"
#include "tessera/hal/caption.hpp"
#include "tessera/hal/cursor.hpp"
#include "tessera/hal/display.hpp"
#include "tessera/hal/event_reason.hpp"
#include "tessera/hal/geometry.hpp"
#include "tessera/hal/refresh_rate.hpp"
#include "tessera/hal/window_backdrop.hpp"
#include "tessera/hal/window_desc.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace tessera::ui {
class InputQueue;
struct InputEvent; // forward-declared: post_input_event takes only a const reference
} // namespace tessera::ui

namespace tessera::hal {

// Display cutouts (notch, punch-hole, camera island): insets say how much to avoid, the rectangles
// say where (docs/specs/os-events-v1.md section 2). `count == 0` is every platform that reports
// none; entries past `count` are zero. Build one with `make_cutout_info`.
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
    // Hinge rectangle, logical pixels, window coordinates; zero when there is no fold. The angle is
    // degrees from the platform's hinge sensor (0 closed, 180 flat), `kUnknownHingeAngle` when
    // nothing measured one; it is independent of `state`, so a flat fold still has an angle.
    WindowRect hinge_bounds;
    float hinge_angle_degrees = kUnknownHingeAngle;
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
    // this from its own capability (DWM composition, an X11 compositing manager and 32-bit visual,
    // intrinsic on Wayland), and a false value here is the opaque fallback.
    bool transparent = false;
    // True when the requested backdrop or translucency could not be honored: the platform resolved
    // to the opaque fallback, and the driver reports TESSERA_GLASS_WINDOW_UNSUPPORTED once.
    bool backdrop_fallback = false;
    // True while the window is full-screen (window-chrome-v1.md section 13, issue #232). It is the
    // state the window manager resolved, not the last request: a platform that refuses keeps
    // reporting false. The pre-fullscreen bounds are the platform's to restore.
    bool fullscreen = false;
    // Interactive client regions of the custom title bar, in logical pixels. Empty by default,
    // which is the system-chrome answer: every point is client area. The HAL resolves WM_NCHITTEST
    // against this list before app routing (window-chrome-v1.md section 8.1).
    std::array<CaptionRegion, kMaxCaptionRegions> caption_regions{};
    std::uint8_t caption_region_count = 0;
    // The pointer shape the platform resolved (window-chrome-v1.md section 8.5, issue #191): the
    // value actually applied, not the last request, so a fake window and a real one agree.
    CursorShape cursor = CursorShape::Default;

    // --- display capabilities (docs/specs/os-events-v1.md section 2, ADR-023 rule 4) -----------
    // The display this window is on. `kNoDisplayId` when the backend cannot say (a headless or fake
    // window), in which case the per-display fields below are the documented fallbacks.
    DisplayId display = kNoDisplayId;
    // The display's current refresh rate, in Hz. It is the value the platform reports now (not the
    // creation-time `display_refresh_hz()` query), so a `RefreshRateChanged` event carries the new
    // rate in its metrics. `kAssumedDisplayRefreshHz` (60) when the platform cannot report one.
    float refresh_hz = kAssumedDisplayRefreshHz;
    // The usable area of the display the window is on, in the display's logical coordinate space
    // (excludes taskbars/docks/panels). Falls back to the window's client area at the origin when
    // the platform has no work-area query.
    WindowRect work_area;
    // The window's aspect orientation, derived from its logical size when the platform has no
    // rotation source (which is every desktop backend).
    Orientation orientation = Orientation::Landscape;
    // The display's subpixel layout. `Unknown` when the platform has no query (a macOS/Android/iOS
    // build, an X11 server without RandR) and when the OS does not answer (Win32 failing its
    // ClearType queries, a Wayland output reporting `unknown`); there is no "assumed" bit because
    // `Unknown` already says so, like `ColorGamut::Unknown`. Win32 reports the user's desktop-wide
    // ClearType configuration, which is not a per-monitor measurement - see
    // `hal::clear_type_geometry`.
    PixelGeometry pixel_geometry = PixelGeometry::Unknown;
    // ADR-023 rule 7: every capability the platform may not be able to measure carries an
    // "assumed" bit here, so a consumer can never mistake the documented fallback (60 Hz, 1.0) for
    // a measurement. Packed into one byte instead of four separate bools + padding: the metrics are
    // copied into every event, so the padding would otherwise cost 3 bytes per copy.
    struct DisplayAssumptions {
        std::uint8_t refresh_hz : 1;  // the refresh is the 60 Hz fallback, not measured
        std::uint8_t vrr : 1;         // VRR support is unknown, not "off"
        std::uint8_t work_area : 1;   // the work area is the window's client area
        std::uint8_t orientation : 1; // the orientation is derived from the extent
    };
    // All bits set: a default-constructed snapshot has measured nothing.
    DisplayAssumptions assumed{1, 1, 1, 1};
    // The display's non-trivial boolean state, packed for the same reason as `assumed`: three
    // separate bools would cost a byte each plus padding in a struct copied with every event.
    struct DisplayFlags {
        std::uint8_t vrr : 1;                // the display follows the content rate
        std::uint8_t multi_window : 1;       // multi-window split-screen state
        std::uint8_t picture_in_picture : 1; // PiP state
    };
    DisplayFlags flags{};
};

// Budget for the per-event metrics snapshot. `WindowMetrics` is embedded in `WindowEvent`, which
// the platform event ring and the pump path copy, and `OsEvent` copies it again, so its size is a
// hot-path cost, not an ergonomics question: the same guardrail as `kNodeSizeBudget` for `Node`
// (docs/guides/code-style.md). `sizeof(WindowMetrics)` is 380 today, so this leaves 4 bytes of
// headroom; growing past it needs a deliberate decision, not an accidental struct member.
inline constexpr std::size_t kWindowMetricsSizeBudget = 384;

static_assert(
    sizeof(WindowMetrics) <= kWindowMetricsSizeBudget,
    "WindowMetrics is copied per event and per pump; keep it within kWindowMetricsSizeBudget");

enum class WindowEventType : std::uint8_t {
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
    // The hinge ANGLE changed without the posture or the hinge rectangle changing (issue #189): the
    // user is part-way through folding. The metrics carry `fold.hinge_angle_degrees`. Separate from
    // `FoldChanged` because the angle is continuous where a posture change is a discrete relayout.
    HingeAngleChanged,
    // --- display capabilities (docs/specs/os-events-v1.md section 2) --------------------------
    // The display's refresh rate changed (a mode change, a move to another monitor, a VRR
    // renegotiation). The metrics carry the new `refresh_hz`/`vrr`. Coalesces into the newest.
    RefreshRateChanged,
    // The window's display work area changed (a taskbar moved, a dock appeared or hid). The metrics
    // carry the new `work_area`. Coalesces into the newest.
    WorkAreaChanged,
    // A display was added to or removed from the desktop. The metrics describe the display the
    // window is on now, so a window that lost its monitor to a hotplug reports its new one.
    DisplayAdded,
    DisplayRemoved,
    // An existing display changed (its mode, position or scale) without being added or removed.
    // Coalesces into the newest.
    DisplayChanged,
    // The window moved to a display with another device scale, but its own `dpi_scale` did not
    // change (a same-scale monitor swap, a compositor scale change). The metrics carry the new
    // `display`/`scale`.
    ScaleChanged,
    // The OS color-scheme preference changed (issue #187): the user switched a light/dark theme in
    // the desktop's settings. It is the `ThemeChanged` member of the normalized taxonomy
    // (docs/specs/os-events-v1.md section 2) and, like `SystemSettingsChanged`, carries no value:
    // the owner re-reads `Window::appearance()` at the tick boundary. Windows reports it through
    // `WM_THEMECHANGED`/`WM_SETTINGCHANGE`, so both platforms have one event for it.
    ThemeChanged,
};

// The window-system family a platform window belongs to. It is the one fact the renderer needs to
// build the matching presentation surface, and it keeps the opaque NativeWindowHandle handles
// unambiguous: display/window mean different OS objects per family (issue #155). The "no native
// presentation surface" enumerator is `NoWindow`, not `None`: <X11/Xlib.h> defines `None` as a
// macro, and this header is included next to Xlib by the Linux backends.
enum class WindowSystem : std::uint8_t {
    NoWindow,
    Win32,
    Xlib,
    Wayland,
    // Android: `native_handle().window` is an `ANativeWindow*` and `display` is unused. The surface
    // is created with `vkCreateAndroidSurfaceKHR` (see rhi::SurfacePlatform::Android).
    Android,
};

// The number of `WindowEventType` enumerators. The rules table is indexed by the enumerator value
// and compile-time checked against this count, so an enumerator added without a row fails the
// build.
inline constexpr std::size_t kWindowEventTypeCount = 20;

// What a window event is about, above its concrete `WindowEventType`: section 3's fixed delivery
// order is these categories in ascending order. Keeping the category separate from the concrete
// type means a new event type sorts correctly the moment its category is known, and the loop's
// ordering rule is one comparison instead of a table of types.
enum class WindowEventCategory : std::uint8_t {
    // Created/shown/hidden/closed/focused/blurred/minimized: lifecycle transitions.
    Lifecycle,
    // Resize/move/DPI/display/scale/refresh/work area: geometry and display capabilities.
    Display,
    // Insets, IME, fold: geometry that is not the display's.
    Geometry,
    // Reduced motion and other OS user settings.
    Settings,
};

// The category `type` belongs to. An out-of-range value is `Settings`, which sorts last: a value
// this build does not understand can never jump ahead of a lifecycle transition.
[[nodiscard]] WindowEventCategory window_event_category(WindowEventType type) noexcept;

// The position of `type`'s category in the fixed delivery order (Lifecycle 0 ... Settings 3).
[[nodiscard]] int window_event_order_rank(WindowEventType type) noexcept;

// True when consecutive events of `type` coalesce into the newest (section 3: resize, move, DPI,
// scale, refresh, insets). Lifecycle transitions never coalesce and return false.
[[nodiscard]] bool window_event_coalesces(WindowEventType type) noexcept;
struct WindowEvent {
    WindowEventType type = WindowEventType::Resize;
    WindowMetrics metrics;
    // Why the event happened (docs/specs/os-events-v1.md section 2). A platform records it when it
    // knows; coalescing keeps the newest non-`Unspecified` reason instead of dropping it.
    EventReason reason = EventReason::Unspecified;
    // Monotonically increasing per window; a value at or below the newest delivered one is stale
    // and must be discarded (section 3). 0 means "not stamped", which is never stale.
    std::uint64_t sequence = 0;
};

// Answers one WM_GETOBJECT (or the platform equivalent) for the window. `object_id` is the OS
// object id; `raw_provider` receives the opaque raw UI Automation root provider the platform hands
// to its OS call (UiaReturnRawElementProvider on Windows). The callback runs on the window's thread
// inside message dispatch and must never block or call UIA: it only reads a pointer another thread
// published. Returning false means "no root ready yet", and the platform falls back to its default
// handling (DefWindowProc, the MSAA default bridge) so a starting or failed bridge never stalls.
struct AccessibilityRoot {
    void* context = nullptr;
    bool (*answer)(void* context, std::intptr_t object_id, void*& raw_provider) = nullptr;
};

// Client-area size is logical pixels; the platform converts it to physical pixels with the
// per-monitor DPI scale. A zero width or height is invalid.
// Opaque identity of a native presentation surface the CALLER owns, for platforms where the OS
// hands the app a surface instead of letting it create a window (Android's `ANativeWindow*`).
// Defined in its own header so this one stays inside its size budget; see
// `tessera/hal/native_surface_token.hpp` for the ownership rule.

// Platform window seam. Implementations live under src/platform/<os>/ and own every OS handle;
// this header stays OS-free so UI and rendering code can depend on it.
class Window {
public:
    virtual ~Window() = default;

    [[nodiscard]] virtual WindowMetrics metrics() const = 0;

    // The window-system family this window was created by. It tells the renderer which presentation
    // surface to build from native_handle() without guessing from the environment, which matters on
    // a Linux box where both X11 and Wayland are available (issue #155). The default `NoWindow` is
    // the answer for backends with no native presentation surface (offscreen/null or a platform
    // stub).
    [[nodiscard]] virtual WindowSystem window_system() const noexcept {
        return WindowSystem::NoWindow;
    }

    // The refresh rate of the display the window is on, in Hz, as the platform currently reports
    // it. `kUnknownRefreshHz` (0) is the documented "unknown" answer: no display, a platform or
    // server that cannot answer, a failed query, or a report outside the sane bounds of
    // tessera/hal/refresh_rate.hpp. It never guesses and never throws.
    //
    // It is not a per-frame query: the app window host reads it once, when the window is created,
    // to resolve WindowOptions::refresh_hz (an explicit option still wins). A monitor change is not
    // tracked in this slice; the OS-event display-capabilities item owns that.
    [[nodiscard]] virtual double display_refresh_hz() const noexcept {
        return kUnknownRefreshHz;
    }

    // Copies the current display topology into `displays` and returns the count written, at most
    // `kMaxDisplays`. `DisplayInfo` is the per-monitor list; `WindowMetrics` carries only the
    // capabilities of the display this window is on, so a caller that needs the others reads them
    // here. A platform without a topology query leaves the span untouched and returns 0, which is
    // the documented "no per-monitor information" answer, never a fabricated monitor.
    //
    // It is a state query, not an event: a `DisplayAdded`/`DisplayRemoved`/`DisplayChanged` event
    // says the list is stale, and the loop re-reads it at that tick's boundary.
    virtual std::size_t displays(std::span<DisplayInfo> displays) const {
        (void)displays;
        return 0;
    }

    // The OS appearance preferences this window mirrors: color scheme and reduced motion
    // (issue #187, `tessera/hal/appearance.hpp`). It is the value the platform measured LAST, not a
    // query: every platform that has a source keeps a normalized snapshot and a change arrives as
    // `ThemeChanged`/`SystemSettingsChanged`, so the owner re-reads it at that tick's boundary
    // instead of polling. The read is cheap and non-blocking (a lock-free load), so it is safe on
    // the frame path; the platform does any I/O on its own thread. The default is the documented
    // fallback (light, motion enabled, both recorded as assumed) for platforms with no source.
    [[nodiscard]] virtual AppearancePreference appearance() const noexcept {
        return assumed_appearance();
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
    // queue is not owned: it must outlive the window or be reset with nullptr first. Without a
    // queue input is dropped; the default does nothing.
    virtual void set_input_queue(ui::InputQueue* queue) {
        (void)queue;
    }

    // Offers one platform input event to the window's translator and returns true when it produced
    // at least one queued `ui::InputEvent`. `event` is opaque (Android: `AInputEvent*`), so no
    // platform type reaches this header (ADR-020 rule 1). False by default; window thread only.
    virtual bool translate_input(const void* event) {
        (void)event;
        return false;
    }

    // Queues one already-normalized event, for a platform whose input the HOST translates because
    // the platform reports it as an object (Android's Java `MotionEvent`). False with no queue.
    virtual bool post_input_event(const ui::InputEvent& event) {
        (void)event;
        return false;
    }

    // The platform's window-metrics signals, for a platform that reports them through a callback
    // the host does not own (Android's `WindowInsets`/`DisplayCutout`/`FoldingFeature` over JNI).
    // Values are PHYSICAL pixels; the backend converts them to logical ones. Cutouts keep at most
    // `kMaxCutouts`; a fold is ignored unless HalfOpened. `set_physical_fold` also carries the
    // hinge angle, which is independent of the posture and may arrive alone.
    virtual void set_physical_insets(const PhysicalInsets& system_bars, const PhysicalInsets& ime,
                                     const PhysicalInsets& gestures) {
        (void)system_bars;
        (void)ime;
        (void)gestures;
    }
    virtual void set_physical_cutouts(std::span<const PhysicalRect> rects) {
        (void)rects;
    }
    virtual void set_physical_fold(const PhysicalFold& fold) {
        (void)fold;
    }
    // The platform's hinge sensor reported an angle in degrees (0 closed, 180 flat), or
    // `kUnknownHingeAngle` when the sensor went away. A separate seam from `set_physical_fold`
    // because it is a separate source at a separate rate: the sensor fires while the user is
    // mid-fold, when the posture has not changed yet.
    virtual void set_physical_hinge_angle(float degrees) {
        (void)degrees;
    }
    // The presentation surface changed extent (a fold switching to the cover display, a rotation),
    // in physical pixels. A backend that re-reads its own surface ignores this; Android cannot,
    // because the ANativeWindow it adopted keeps reporting the old extent.
    virtual void set_surface_extent(std::uint32_t width_px, std::uint32_t height_px) {
        (void)width_px;
        (void)height_px;
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
    // It never blocks, throws, or fails: a platform that cannot honor the request leaves the window
    // as it was, `metrics().fullscreen` keeps telling the truth, and the caller reports one
    // TESSERA_WINDOW_UNSUPPORTED diagnostic. The default is a no-op.
    virtual void set_fullscreen(bool fullscreen) {
        (void)fullscreen;
    }

    // False when full-screen cannot be requested at all on this platform, so a caller can report
    // the unsupported path once instead of calling set_fullscreen() and re-reading the metrics.
    // True does not promise that a request succeeds: a window manager may still refuse it, which
    // the metrics report. The default is false.
    [[nodiscard]] virtual bool supports_fullscreen() const noexcept {
        return false;
    }

    // Publishes the interactive regions of the framework's custom title bar (window-chrome-v1.md
    // section 8.1); an empty span returns the window to the system-chrome answer where every point
    // is client area. The regions are in logical pixels and are copied: the platform keeps them for
    // its non-client hit test, and the caller may pass a stack buffer. They also appear in
    // WindowMetrics::caption_regions, so a fake window and a real one report the same list. This is
    // a framework-only seam (WindowChrome); app code never calls it. The default stores nothing,
    // for platforms with no custom caption.
    virtual void set_caption_regions(std::span<const CaptionRegion> regions) {
        (void)regions;
    }

    // Requests the native pointer shape (window-chrome-v1.md section 8.5, issue #191). The app
    // resolves the hovered scene node's CSS cursor into a `CursorShape` and calls this only when
    // the wanted shape changed, so a static hover costs no platform call. It never blocks, throws
    // or fails: the platform applies the shape on its next event-loop iteration and
    // `metrics().cursor` reports what it resolved; an unsupported shape or platform leaves the
    // pointer as it was and the caller reports one TESSERA_WINDOW_CURSOR_UNSUPPORTED diagnostic.
    //
    // X11 applies it with `XDefineCursor` (Xlib buffers the request, so not a blocking round trip
    // in the ADR-021 sense); Win32 records the shape and lets the next `WM_SETCURSOR` install it,
    // because the OS re-asserts the class cursor on every non-client transition.
    virtual void set_cursor(CursorShape shape) {
        (void)shape;
    }

    // False when the pointer shape cannot be requested at all on this platform, so a caller can
    // report the unsupported path once instead of calling set_cursor() and re-reading the metrics.
    // The default is false.
    [[nodiscard]] virtual bool supports_cursor() const noexcept {
        return false;
    }

    // Asks the runtime for a frame; coalesced, so repeated calls before a frame are harmless.
    virtual void request_redraw() = 0;

    // Requests graceful shutdown. Implementations emit WindowEventType::Close; calling twice is
    // harmless.
    virtual void close() = 0;

    // Renderer/swapchain glue only. Win32 sets window (HWND); X11 sets display (Display*) and
    // window (Window id); Wayland sets display (wl_display*) and window (wl_surface*). See
    // WindowSystem.
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
//
// Linux backend selection (issue #155): when the build has the Wayland backend and the process runs
// under a Wayland session (`WAYLAND_DISPLAY` is set), the Wayland backend is created first; a
// failed Wayland connection falls back to X11 when `DISPLAY` is set, so an X11 session and a CI
// Xvfb run behave exactly as before. `TESSERA_WINDOW_BACKEND=x11|wayland` pins the backend for a
// diagnostic or a test (no fallback). Nothing changes on Windows or macOS.
std::unique_ptr<Window> create_window(const WindowDesc& desc);

} // namespace tessera::hal
