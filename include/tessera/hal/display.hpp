// SPDX-License-Identifier: MIT
//
// Display identity and per-monitor capabilities (docs/specs/os-events-v1.md section 2 and
// docs/design/hal.md section 2; ADR-023 rules 4 and 7).
//
// `WindowMetrics` carries the *window's* display capabilities (scale, refresh, work area, the id of
// the display it is on). The per-monitor list is a separate `DisplayInfo` sequence published by
// `hal::Window::displays()`, never a field of the per-event metrics: a window on a 3-monitor
// desktop copies its own capabilities into every event, not the whole topology.
//
// Every value here is platform-neutral and allocation-free: a platform fills the list from its OS
// API, and the engine only ever sees these ordinary value types (ADR-020: no HWND, HMONITOR,
// xcb_randr_monitor_info_t or XRRMonitorInfo crosses the HAL boundary).

#pragma once

#include "tessera/hal/geometry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace tessera::hal {

// Opaque window and display identities. 0 is "no window"/"no display": the id a backend that
// cannot report one leaves behind, so a consumer never treats a default value as a real target.
using WindowId = std::uint64_t;
using DisplayId = std::uint64_t;

inline constexpr WindowId kNoWindowId = 0;
inline constexpr DisplayId kNoDisplayId = 0;
// The framework creates one window per host today (docs/design/app-host.md section 5), so the
// normalized event model's window id for this window is the fixed non-zero primary id. Multi-window
// routing (issue #228) will hand out real ids; until then every `OsEvent` carries this one.
inline constexpr WindowId kPrimaryWindowId = 1;

// Upper bound of `hal::Window::displays()`. A desktop reports a handful of monitors; a longer list
// is truncated, so the platform's fixed array can never overrun.
inline constexpr std::size_t kMaxDisplays = 8;

// Upper bound of a display name in bytes, including the terminating NUL. The list is copied by
// value (a fake swaps it in a test, a window caches it), so a fixed buffer keeps a `DisplayInfo`
// trivially copyable and allocation-free; a longer OS name is truncated.
inline constexpr std::size_t kMaxDisplayNameBytes = 32;

// The display's aspect orientation, as the metrics report it. A square/unknown display is
// landscape: it is the default and the only value a desktop without a rotation source reports.
enum class Orientation : std::uint8_t { Landscape, Portrait };

// The display's color gamut. `Unknown` is the honest answer for a backend with no color query; no
// desktop backend fills this in V1 (the HDR/color rows of the OS event tables are post-V1).
enum class ColorGamut : std::uint8_t { Unknown, Srgb, DisplayP3, Rec2020 };

// One monitor. `bounds` is the monitor rectangle in *logical* pixels in the virtual-desktop
// coordinate space (the space a window's position lives in), so a 200 %-scaled 4K panel presents
// 1920x1080 logical pixels with `physical_width/height` still 3840x2160.
struct DisplayInfo {
    DisplayId id = kNoDisplayId;
    // UTF-8, NUL-terminated; the OS device or connector name (Win32 `\\.\DISPLAY1`, X11 monitor
    // name). Guaranteed terminated: `display_info_name` reads at most `kMaxDisplayNameBytes`.
    std::array<char, kMaxDisplayNameBytes> name{};
    WindowRect bounds;
    std::uint32_t physical_width = 0;
    std::uint32_t physical_height = 0;
    // Device scale (dpi / 96). 1.0 when the platform has no per-monitor scale query, together with
    // `scale_assumed == true` so the fallback is recorded rather than passed off as measured
    // (ADR-023 rule 7).
    float scale = 1.0f;
    // The display's current refresh rate. 60 Hz when the platform cannot report one, together with
    // `refresh_hz_assumed == true`; the same rule as `WindowMetrics::refresh_hz`.
    float refresh_hz = 60.0f;
    bool refresh_hz_assumed = false;
    bool scale_assumed = false;
    bool primary = false;
    bool vrr = false;
    bool hdr = false;
    ColorGamut gamut = ColorGamut::Unknown;
};

// The NUL-terminated name of `display` as a view. Never reads past the buffer.
[[nodiscard]] std::string_view display_info_name(const DisplayInfo& display) noexcept;

// Landscape when the logical size is at least as wide as it is tall, portrait otherwise. A square
// display (and a zero-size window, which a minimized platform window reports) is landscape: the
// orientation has no meaningful answer there and landscape is the documented default.
[[nodiscard]] Orientation orientation_from_extent(std::uint32_t logical_width,
                                                  std::uint32_t logical_height) noexcept;

// The human-readable name of `orientation` ("landscape"/"portrait"); used by the deterministic
// metrics dump and diagnostics.
[[nodiscard]] std::string_view orientation_name(Orientation orientation) noexcept;

// The display in `displays` whose `id` is `id`, or nullptr. Linear over at most `kMaxDisplays`.
[[nodiscard]] const DisplayInfo* find_display(std::span<const DisplayInfo> displays,
                                              DisplayId id) noexcept;

// The documented fallback values (docs/design/hal.md section 5, ADR-023 rule 7). A platform that
// cannot report a capability uses these and records the assumption; a guess is never presented as a
// measurement.
inline constexpr float kAssumedDisplayScale = 1.0f;
inline constexpr float kAssumedDisplayRefreshHz = 60.0f;

// True when an OS-reported device scale can be used as-is: finite and positive. A zero, negative,
// NaN or infinite report is not a scale, so the caller keeps `kAssumedDisplayScale` and marks
// `scale_assumed`.
[[nodiscard]] bool display_scale_usable(float reported_scale) noexcept;

// The scale a reported value resolves to: the report when usable, `kAssumedDisplayScale` otherwise.
// The caller pairs it with `display_scale_usable` to set the assumption flag.
[[nodiscard]] float resolve_display_scale(float reported_scale) noexcept;

// The device scale from a DPI value (`dpi / 96`), which is how Win32 and X11 describe a monitor.
// A non-finite or non-positive DPI resolves through `resolve_display_scale` to 1.0.
[[nodiscard]] float display_scale_from_dpi(float dpi) noexcept;

// The work area a reported rectangle resolves to. A non-finite or empty rectangle has no usable
// meaning, so the fallback is the window's own client area at the origin: a layout that clamps to
// the work area then degrades to "the window", never to zero.
[[nodiscard]] WindowRect resolve_work_area(const WindowRect& reported, std::uint32_t logical_width,
                                           std::uint32_t logical_height) noexcept;

// True when an orientation is one of the enumerators above (a wire value from a script or a corrupt
// platform answer is not).
[[nodiscard]] bool orientation_is_known(Orientation orientation) noexcept;

} // namespace tessera::hal
