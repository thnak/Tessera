// SPDX-License-Identifier: MIT

// The normalized OS event model (docs/specs/os-events-v1.md sections 2-3, ADR-023 rules 5 and 7):
// the display-capability value rules, the event reason merge, and the window-path taxonomy -
// category (the fixed delivery order), coalescing, names and the total `os_event_type_for` mapping.
//
// Pure CPU and display-free: the platform halves are covered by the window tests (a real Win32 or
// X11 window) and by the synthetic scripts in `os_event_display`. Every enumerator count here is a
// compile-time assertion as well, so an enum grown without a table row cannot ship.

#include "tessera/hal/display.hpp"
#include "tessera/hal/event_reason.hpp"
#include "tessera/hal/os_event.hpp"
#include "tessera/hal/refresh_rate.hpp"
#include "tessera/hal/window.hpp"

#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>

namespace {

using namespace tessera;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "os_event: " << message << "\n";
        ++failures;
    }
    return condition;
}

void check_display_scale() {
    // ADR-023 rule 7: a missing/unusable scale resolves to 1.0 and the caller records it, so the
    // usable predicate is what tells a measured value from an assumed one.
    expect(hal::display_scale_usable(1.0f) && hal::display_scale_usable(2.0f) &&
               hal::display_scale_usable(1.25f),
           "a finite positive scale is usable");
    expect(!hal::display_scale_usable(0.0f) && !hal::display_scale_usable(-2.0f) &&
               !hal::display_scale_usable(std::numeric_limits<float>::quiet_NaN()) &&
               !hal::display_scale_usable(std::numeric_limits<float>::infinity()),
           "zero, negative, NaN and infinity are not scales");
    expect(hal::resolve_display_scale(0.0f) == hal::kAssumedDisplayScale,
           "a zero scale resolves to the documented 1.0 fallback");
    expect(hal::resolve_display_scale(std::numeric_limits<float>::infinity()) ==
               hal::kAssumedDisplayScale,
           "an infinite scale resolves to the documented 1.0 fallback");
    expect(hal::resolve_display_scale(1.5f) == 1.5f, "a usable scale is returned unchanged");
    // The DPI conversion the platforms use: 96 dpi is 1.0, 192 is 2.0, and a broken DPI is 1.0.
    expect(hal::display_scale_from_dpi(96.0f) == 1.0f, "96 dpi is scale 1.0");
    expect(hal::display_scale_from_dpi(192.0f) == 2.0f, "192 dpi is scale 2.0");
    expect(hal::display_scale_from_dpi(0.0f) == hal::kAssumedDisplayScale,
           "a zero dpi resolves to the fallback");
    expect(hal::display_scale_from_dpi(std::numeric_limits<float>::quiet_NaN()) ==
               hal::kAssumedDisplayScale,
           "a NaN dpi resolves to the fallback");
}

void check_work_area() {
    // A usable work area is kept verbatim; an empty/non-finite one degrades to the window's client
    // area, never to zero (a layout that clamps to it must not clamp everything away).
    const hal::WindowRect usable{0.0f, 24.0f, 1920.0f, 1016.0f};
    const hal::WindowRect resolved = hal::resolve_work_area(usable, 1280, 720);
    expect(resolved.x == usable.x && resolved.y == usable.y && resolved.width == usable.width &&
               resolved.height == usable.height,
           "a usable work area is kept");
    const hal::WindowRect empty = hal::resolve_work_area({}, 1280, 720);
    expect(empty.x == 0.0f && empty.y == 0.0f && empty.width == 1280.0f && empty.height == 720.0f,
           "an empty work area falls back to the window");
    const hal::WindowRect bad = hal::resolve_work_area(hal::WindowRect{0, 0, -5, 10}, 800, 600);
    expect(bad.width == 800.0f && bad.height == 600.0f, "a negative work area falls back");
    const hal::WindowRect nan = hal::resolve_work_area(
        hal::WindowRect{std::numeric_limits<float>::quiet_NaN(), 0, 10, 10}, 800, 600);
    expect(nan.width == 800.0f, "a NaN work area falls back");
}

void check_orientation() {
    expect(hal::orientation_from_extent(1920, 1080) == hal::Orientation::Landscape,
           "a wide window is landscape");
    expect(hal::orientation_from_extent(1080, 1920) == hal::Orientation::Portrait,
           "a tall window is portrait");
    // A square (and a zero-size minimized window) has no meaningful orientation: landscape is the
    // documented default.
    expect(hal::orientation_from_extent(800, 800) == hal::Orientation::Landscape,
           "a square window defaults to landscape");
    expect(hal::orientation_from_extent(0, 0) == hal::Orientation::Landscape,
           "a zero-size window defaults to landscape");
    expect(hal::orientation_name(hal::Orientation::Landscape) == "landscape", "landscape name");
    expect(hal::orientation_name(hal::Orientation::Portrait) == "portrait", "portrait name");
    expect(hal::orientation_is_known(hal::Orientation::Landscape) &&
               hal::orientation_is_known(hal::Orientation::Portrait),
           "every enumerator is known");
    expect(!hal::orientation_is_known(static_cast<hal::Orientation>(200)),
           "an out-of-range orientation is not known");
}

void check_display_names() {
    hal::DisplayInfo display;
    expect(hal::display_info_name(display).empty(), "a default name is empty");
    const char* name = "HDMI-1";
    std::memcpy(display.name.data(), name, std::strlen(name) + 1);
    expect(hal::display_info_name(display) == "HDMI-1", "the name is read as a view");
    // An unterminated buffer is read up to its capacity, never past it.
    hal::DisplayInfo full;
    std::memset(full.name.data(), 'x', full.name.size());
    expect(hal::display_info_name(full).size() == full.name.size(),
           "a full buffer is read to its capacity");
}

void check_find_display() {
    hal::DisplayInfo a;
    a.id = 7;
    hal::DisplayInfo b;
    b.id = 9;
    const hal::DisplayInfo displays[] = {a, b};
    expect(hal::find_display(displays, 9) == &displays[1], "the matching display is found");
    expect(hal::find_display(displays, 7) == &displays[0], "the first display is found");
    expect(hal::find_display(displays, 11) == nullptr, "an absent display is not found");
    // 0 is "no display", never a match on a default-constructed entry.
    hal::DisplayInfo none;
    expect(hal::find_display(std::span<const hal::DisplayInfo>(&none, 1), hal::kNoDisplayId) ==
               nullptr,
           "the no-display id never matches");
}

void check_event_reasons() {
    using hal::EventReason;
    // A later generic notification never erases the specific cause that started the run.
    expect(hal::merge_event_reason(EventReason::Dpi, EventReason::Unspecified) == EventReason::Dpi,
           "an unspecified later reason keeps the earlier one");
    expect(hal::merge_event_reason(EventReason::Dpi, EventReason::System) == EventReason::Dpi,
           "a generic system reason does not erase a specific cause");
    expect(hal::merge_event_reason(EventReason::System, EventReason::Dpi) == EventReason::Dpi,
           "a later specific reason wins");
    expect(hal::merge_event_reason(EventReason::Unspecified, EventReason::Display) ==
               EventReason::Display,
           "the first recorded reason is kept");
    expect(hal::merge_event_reason(EventReason::User, EventReason::Hotplug) == EventReason::Hotplug,
           "two specific reasons keep the newest");
    expect(hal::event_reason_name(EventReason::Dpi) == "dpi", "the dpi reason name");
    expect(hal::event_reason_name(EventReason::Hotplug) == "hotplug", "the hotplug reason name");
    expect(hal::event_reason_name(static_cast<EventReason>(250)) == "unknown",
           "an out-of-range reason is unknown");
    expect(hal::event_reason_is_known(EventReason::Programmatic), "the last reason is known");
    expect(!hal::event_reason_is_known(static_cast<EventReason>(250)),
           "an out-of-range reason is not known");
}

void check_taxonomy() {
    using hal::OsEventType;
    // The count the table is compile-time checked against must equal the enum's last value + 1.
    expect(static_cast<std::size_t>(OsEventType::ShutdownRequested) + 1 == hal::kOsEventTypeCount,
           "kOsEventTypeCount matches the enumerator count");
    expect(hal::kOsEventTypeCount == 48, "the documented enumerator count");
    expect(hal::os_event_is_known(OsEventType::WindowCreated), "the first type is known");
    expect(hal::os_event_is_known(OsEventType::ShutdownRequested), "the last type is known");
    expect(!hal::os_event_is_known(static_cast<OsEventType>(200)),
           "an out-of-range type is not known");

    // Every enumerator has a distinct, non-empty spec name (the table's uniqueness check mirrors
    // it).
    for (std::size_t index = 0; index < hal::kOsEventTypeCount; ++index) {
        const auto type = static_cast<OsEventType>(index);
        const std::string_view name = hal::os_event_type_name(type);
        if (!expect(!name.empty() && name != "unknown", "every enumerator has a name")) {
            return;
        }
    }
    expect(hal::os_event_type_name(static_cast<OsEventType>(200)) == "unknown",
           "an out-of-range type reports unknown");

    // The fixed order of section 3: lifecycle, then display/DPI/orientation, then fold/posture,
    // then settings, then power/system. The rank is what the loop sorts by.
    expect(hal::os_event_order_rank(OsEventType::WindowResized) == 0, "lifecycle ranks first");
    expect(hal::os_event_order_rank(OsEventType::DpiChanged) == 1, "display ranks second");
    expect(hal::os_event_order_rank(OsEventType::FoldChanged) == 2, "fold ranks third");
    expect(hal::os_event_order_rank(OsEventType::ReducedMotionChanged) == 3,
           "settings rank fourth");
    expect(hal::os_event_order_rank(OsEventType::Suspend) == 4, "power ranks fifth");
    expect(hal::os_event_order_rank(OsEventType::ShutdownRequested) == 5, "system ranks last");
    expect(hal::os_event_category(OsEventType::WindowResized) == hal::OsEventCategory::Lifecycle,
           "the resize category");
    expect(hal::os_event_category(OsEventType::RefreshRateChanged) == hal::OsEventCategory::Display,
           "the refresh category");
    // An unknown value sorts last so it can never jump ahead of a lifecycle transition.
    expect(hal::os_event_order_rank(static_cast<OsEventType>(200)) == 5,
           "an unknown type sorts last");

    // Section 3's coalescing set: resize, move, DPI, scale, orientation, refresh, work area.
    // Lifecycle transitions are never dropped.
    expect(hal::os_event_coalesces(OsEventType::WindowResized), "resize coalesces");
    expect(hal::os_event_coalesces(OsEventType::DpiChanged), "dpi coalesces");
    expect(hal::os_event_coalesces(OsEventType::RefreshRateChanged), "refresh coalesces");
    expect(hal::os_event_coalesces(OsEventType::WorkAreaChanged), "work area coalesces");
    expect(!hal::os_event_coalesces(OsEventType::WindowClosed), "close never coalesces");
    expect(!hal::os_event_coalesces(OsEventType::Suspend), "suspend never coalesces");
    expect(!hal::os_event_coalesces(static_cast<OsEventType>(200)),
           "an unknown type does not coalesce");

    // Rule 4: a visible change is a wake source; a state-only event is not.
    expect(hal::os_event_requests_frame(OsEventType::WindowResized), "resize wakes the loop");
    expect(hal::os_event_requests_frame(OsEventType::RefreshRateChanged), "refresh wakes the loop");
    expect(!hal::os_event_requests_frame(OsEventType::MemoryPressure),
           "memory pressure is state only");
    expect(!hal::os_event_requests_frame(OsEventType::LocaleChanged), "locale is state only");
    expect(!hal::os_event_requests_frame(static_cast<OsEventType>(200)),
           "an unknown event never wakes the loop");
}

void check_window_path() {
    using hal::WindowEventType;
    expect(static_cast<std::size_t>(WindowEventType::ThemeChanged) + 1 ==
               hal::kWindowEventTypeCount,
           "kWindowEventTypeCount matches the window enumerator count");
    expect(hal::kWindowEventTypeCount == 19, "the documented window enumerator count");
    expect(hal::window_event_type_is_known(WindowEventType::Resize), "resize is known");
    expect(hal::window_event_type_is_known(WindowEventType::ScaleChanged), "scale is known");
    expect(!hal::window_event_type_is_known(static_cast<WindowEventType>(200)),
           "an out-of-range window type is not known");

    // The mapping is total: every enumerator maps to a known, taxonomically valid OsEventType.
    for (std::size_t index = 0; index < hal::kWindowEventTypeCount; ++index) {
        const auto type = static_cast<WindowEventType>(index);
        const hal::OsEventType mapped = hal::os_event_type_for(type);
        if (!expect(hal::os_event_is_known(mapped),
                    "every window type maps to a known OsEventType")) {
            return;
        }
    }
    // Spot checks the platforms rely on.
    expect(hal::os_event_type_for(WindowEventType::Resize) == hal::OsEventType::WindowResized,
           "Resize maps to WindowResized");
    expect(hal::os_event_type_for(WindowEventType::DpiChange) == hal::OsEventType::DpiChanged,
           "DpiChange maps to DpiChanged");
    expect(hal::os_event_type_for(WindowEventType::RefreshRateChanged) ==
               hal::OsEventType::RefreshRateChanged,
           "RefreshRateChanged maps to itself");
    expect(hal::os_event_type_for(WindowEventType::DisplayAdded) == hal::OsEventType::DisplayAdded,
           "DisplayAdded maps to itself");
    expect(hal::window_event_type_name(WindowEventType::DpiChange) == "DpiChange",
           "the window path name is the spec spelling");
    expect(hal::window_event_type_name(static_cast<WindowEventType>(200)) == "unknown",
           "an out-of-range window type reports unknown");

    // The delivery order is the normalized taxonomy's (section 3): lifecycle, then display/DPI/
    // orientation, then fold/posture, then settings. The window path is a projection of that model,
    // so it ranks by the `OsEventType` each window type maps to.
    expect(hal::window_event_order_rank(WindowEventType::Close) == 0, "close ranks first");
    expect(hal::window_event_order_rank(WindowEventType::Resize) == 0,
           "a resize is a lifecycle transition");
    expect(hal::window_event_order_rank(WindowEventType::RefreshRateChanged) == 1,
           "refresh ranks with display");
    expect(hal::window_event_order_rank(WindowEventType::SystemSettingsChanged) == 3,
           "settings rank last");
    expect(hal::window_event_order_rank(WindowEventType::ThemeChanged) == 3,
           "a color-scheme change ranks with the other settings");
    expect(hal::os_event_type_for(WindowEventType::ThemeChanged) == hal::OsEventType::ThemeChanged,
           "ThemeChanged maps to the taxonomy's ThemeChanged");
    expect(!hal::window_event_coalesces(WindowEventType::ThemeChanged),
           "a settings transition never coalesces");
    expect(hal::window_event_coalesces(WindowEventType::Resize), "resize coalesces");
    expect(hal::window_event_coalesces(WindowEventType::RefreshRateChanged), "refresh coalesces");
    expect(!hal::window_event_coalesces(WindowEventType::Close), "close never coalesces");
}

void check_sequence() {
    hal::OsEventSequence sequence;
    expect(sequence.newest() == 0, "a fresh sequence is 0");
    expect(sequence.next() == 1 && sequence.next() == 2, "the sequence is monotonic");
    expect(sequence.newest() == 2, "newest reports the last issued value");
    // 0 is "not stamped" and is never stale; anything at or below the newest is.
    expect(!hal::os_event_sequence_is_stale(0, 2), "an unstamped sequence is never stale");
    expect(hal::os_event_sequence_is_stale(1, 2), "an older sequence is stale");
    expect(hal::os_event_sequence_is_stale(2, 2), "a repeated sequence is stale");
    expect(!hal::os_event_sequence_is_stale(3, 2), "a newer sequence is not stale");
}

void check_conversion() {
    hal::WindowEvent raw;
    raw.type = hal::WindowEventType::RefreshRateChanged;
    raw.reason = hal::EventReason::Display;
    raw.metrics.display = 42;
    raw.metrics.refresh_hz = 120.0f;
    raw.metrics.fold.state = hal::FoldState::Flat;

    const hal::OsEvent event = hal::to_os_event(raw, 7, 123456);
    expect(event.type == hal::OsEventType::RefreshRateChanged, "the type is mapped");
    expect(event.sequence == 7, "the sequence is stamped by the caller");
    expect(event.timestamp_ns == 123456, "the timestamp is stamped by the caller");
    expect(event.window == hal::kPrimaryWindowId, "the window id is the primary one");
    expect(event.display == 42, "the display id comes from the metrics");
    expect(event.reason == hal::EventReason::Display, "the reason is carried");
    expect(event.metrics.refresh_hz == 120.0f, "the metrics are carried");
    expect(event.fold.state == hal::FoldState::Flat, "the fold snapshot is carried");
    expect(!event.handled, "a fresh event is unhandled");
}

void check_coalescing() {
    // A tick's raw events: a resize storm, a focus event, a DPI change and another resize. Only the
    // coalescing types collapse; the lifecycle event between them is a real ordering boundary.
    hal::WindowEvent resize;
    resize.type = hal::WindowEventType::Resize;
    resize.reason = hal::EventReason::User;
    resize.metrics.logical_width = 100;

    hal::WindowEvent focus;
    focus.type = hal::WindowEventType::Focus;
    focus.reason = hal::EventReason::User;

    hal::WindowEvent dpi;
    dpi.type = hal::WindowEventType::DpiChange;
    dpi.reason = hal::EventReason::Dpi;
    dpi.metrics.dpi_scale = 2.0f;

    const hal::WindowEvent raw[] = {resize, resize, focus, dpi, resize};
    hal::WindowEvent out[8]{};
    const std::size_t count = hal::coalesce_window_events(raw, out);
    expect(count == 4, "two adjacent resizes collapse; the rest are kept");

    // The fixed order: every lifecycle event first (both resizes and the focus event, in their
    // original order and still separate runs), then the display event (the DPI change).
    expect(out[0].type == hal::WindowEventType::Resize, "lifecycle coalesces first");
    expect(out[0].metrics.logical_width == 100, "the coalesced resize keeps the newest value");
    expect(out[1].type == hal::WindowEventType::Focus, "the focus event keeps its place");
    expect(out[2].type == hal::WindowEventType::Resize, "the last resize is a new run");
    expect(out[3].type == hal::WindowEventType::DpiChange, "the display event follows lifecycle");
    expect(count == 4, "an interleaved event breaks the coalescing run");

    // The reasons merge: a specific cause survives a later generic notification.
    hal::WindowEvent specific;
    specific.type = hal::WindowEventType::Resize;
    specific.reason = hal::EventReason::Dpi;
    hal::WindowEvent generic;
    generic.type = hal::WindowEventType::Resize;
    generic.reason = hal::EventReason::System;
    const hal::WindowEvent reasons[] = {specific, generic};
    hal::WindowEvent merged[2]{};
    expect(hal::coalesce_window_events(reasons, merged) == 1, "same-type runs coalesce");
    expect(merged[0].reason == hal::EventReason::Dpi,
           "a generic later notification keeps the specific reason");
    expect(hal::coalesce_window_events({}, merged) == 0, "an empty batch is empty");
}

// The Windows ClearType setting -> `PixelGeometry` rules (display-aware-rendering.md section 2):
// the pure mapping, so this check needs no Windows API and no display. The three arguments are the
// values the platform TU reads from `SPI_GETFONTSMOOTHING`, `SPI_GETFONTSMOOTHINGORIENTATION` and
// `SPI_GETFONTSMOOTHINGTYPE`.
void check_clear_type_geometry() {
    using hal::FontSmoothingOrientation;
    using hal::FontSmoothingType;
    using hal::PixelGeometry;

    // The only two configurations that describe a stripe LCD text can use.
    expect(hal::clear_type_geometry(true, FontSmoothingOrientation::Rgb,
                                    FontSmoothingType::ClearType) == PixelGeometry::RgbHorizontal,
           "ClearType with an RGB stripe is RgbHorizontal");
    expect(hal::clear_type_geometry(true, FontSmoothingOrientation::Bgr,
                                    FontSmoothingType::ClearType) == PixelGeometry::BgrHorizontal,
           "ClearType with a BGR stripe is BgrHorizontal");
    // Smoothing off wins over a stale ClearType type: the OS will not blend subpixel coverage.
    expect(hal::clear_type_geometry(false, FontSmoothingOrientation::Rgb,
                                    FontSmoothingType::ClearType) == PixelGeometry::None,
           "smoothing off is None even if ClearType is still selected as the type");
    // Greyscale ("standard") smoothing has no stripe to describe.
    expect(hal::clear_type_geometry(true, FontSmoothingOrientation::Rgb,
                                    FontSmoothingType::Standard) == PixelGeometry::None,
           "greyscale smoothing is None");
    expect(hal::clear_type_geometry(true, FontSmoothingOrientation::Bgr, FontSmoothingType::None) ==
               PixelGeometry::None,
           "a None smoothing type is None");
    // A value this build does not understand is never promoted to a known stripe.
    expect(hal::clear_type_geometry(true, FontSmoothingOrientation::Rgb,
                                    static_cast<FontSmoothingType>(200)) == PixelGeometry::Unknown,
           "an unknown smoothing type is Unknown");
    expect(hal::clear_type_geometry(true, static_cast<FontSmoothingOrientation>(200),
                                    FontSmoothingType::ClearType) == PixelGeometry::Unknown,
           "an unknown stripe orientation is Unknown");
    // Win32 never reports a vertical stripe, and no mapping invents one.
    expect(!hal::pixel_geometry_is_horizontal_stripe(hal::clear_type_geometry(
               true, FontSmoothingOrientation::Rgb, FontSmoothingType::Standard)) &&
               !hal::pixel_geometry_is_horizontal_stripe(hal::clear_type_geometry(
                   false, FontSmoothingOrientation::Bgr, FontSmoothingType::ClearType)),
           "only a ClearType horizontal configuration is an LCD-usable stripe");
}

void check_metrics_assumptions() {
    // ADR-023 rule 7 through the type's defaults: an unmeasured display reports 60 Hz / 1.0 and the
    // flags say so. A consumer that reads only a value must not be able to mistake it for a
    // measurement.
    const hal::WindowMetrics defaults;
    expect(defaults.refresh_hz == hal::kAssumedDisplayRefreshHz,
           "the default refresh is the documented 60 Hz");
    expect(defaults.assumed.refresh_hz, "the default refresh is marked assumed");
    expect(defaults.dpi_scale == hal::kAssumedDisplayScale, "the default scale is 1.0");
    expect(defaults.assumed.vrr, "the default VRR is marked assumed");
    expect(defaults.assumed.work_area, "the default work area is marked assumed");
    expect(defaults.assumed.orientation, "the default orientation is marked assumed");
    expect(defaults.display == hal::kNoDisplayId, "the default display is unknown");
    expect(defaults.orientation == hal::Orientation::Landscape, "the default is landscape");
    expect(hal::kAssumedDisplayRefreshHz == 60.0f, "the documented 60 Hz fallback");
    expect(hal::kAssumedDisplayScale == 1.0f, "the documented 1.0 scale fallback");
    expect(hal::kUnknownRefreshHz == 0.0, "the display-refresh seam's unknown value is 0");
}

} // namespace

int main() {
    check_display_scale();
    check_work_area();
    check_orientation();
    check_display_names();
    check_find_display();
    check_event_reasons();
    check_taxonomy();
    check_window_path();
    check_sequence();
    check_conversion();
    check_coalescing();
    check_metrics_assumptions();
    check_clear_type_geometry();

    if (failures != 0) {
        std::cerr << "os_event: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "os_event: ok\n";
    return 0;
}
