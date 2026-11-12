// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Pure XInput2 (XI2) mapping rules: smooth-scroll valuators, touch contacts, pen/tablet samples and
// the per-device profile the pump fills from XIQueryDevice class records. This header deliberately
// does not include <X11/extensions/XInput2.h>: the valuator numbers, increments, device names and
// raw sample values the Xlib-facing pump mirrors into the plain structs below are the only inputs,
// so every rule here is unit-testable on any platform without a display or an input device.
//
// Scroll sign convention (XI2 2.x spec section 8.4, "Smooth scrolling"): one unit of scrolling is
// the valuator delta named by the axis' ScrollClass increment, and the protocol emulates the legacy
// wheel buttons from it. A negative vertical delta is one "scroll up" unit (the server emulates
// button 4 for it) and a positive horizontal delta is one "scroll right" unit (button 7). Tessera's
// normalized Wheel deltas use the web sign (positive dy scrolls content down, positive dx scrolls
// right), while the core path yields dy = -1 for an up notch (button 4) and dx = +1 for a right
// notch (button 7). The mapping is therefore:
//     dy = +raw_vertical   / vertical_increment
//     dx = +raw_horizontal / horizontal_increment
// with no inversion, and both paths agree on the sign of a notch. A driver's natural-scroll setting
// inverts the raw deltas before they reach the client, so the pump must not invert again.

namespace tessera::hal::x11 {

// ---------------------------------------------------------------------------------------------
// Scroll valuators
// ---------------------------------------------------------------------------------------------

// One normalized wheel unit is one legacy wheel notch (buttons 4..7), matching the core path.
class ScrollAccumulator {
public:
    // An increment <= 0 or non-finite disables the axis: its raw deltas are dropped instead of
    // dividing by zero and producing an infinite or NaN delta. XI 2.1 drivers always publish a
    // positive increment; a device whose ScrollClass is missing (or a stale device record) is the
    // case this guards.
    void set_vertical_increment(float increment);
    void set_horizontal_increment(float increment);

    void reset();

    // Feeds one device-dependent valuator delta in raw units (already sign-corrected by the
    // driver). Returns true when at least one whole wheel unit is due and writes the accumulated
    // deltas, which are whole units with the web sign. The sub-unit remainder is kept, so a slow
    // axis emits no zero-delta event and never drifts: 3 x 0.5 units emit 0, 0, 1 (then 0.5 kept),
    // never 0, 1, 0 or 2. Non-finite deltas are ignored.
    [[nodiscard]] bool add(float raw_vertical, float raw_horizontal, float& out_dx, float& out_dy);

    [[nodiscard]] bool vertical_enabled() const {
        return vertical_.enabled;
    }
    [[nodiscard]] bool horizontal_enabled() const {
        return horizontal_.enabled;
    }

private:
    struct Axis {
        bool enabled = false;
        float increment = 0.0F;
        float pending = 0.0F;
    };

    static void configure(Axis& axis, float increment);
    static bool accumulate(Axis& axis, float raw, float& out);

    Axis vertical_{};
    Axis horizontal_{};
};

// ---------------------------------------------------------------------------------------------
// Touch
// ---------------------------------------------------------------------------------------------

enum class TouchPhase : std::uint8_t { Begin, Update, End, Cancel };

// Typed outcome of one touch sample. `Ignored` is a normal answer (a duplicate begin, or an
// update/end/cancel for a contact that is not live) and never an error.
enum class TouchResult : std::uint8_t { Emitted, Ignored };

// Framework pointer ids for touch contacts start above the mouse/pen range, the same disjoint
// convention the Win32 touch fallback uses. Ids are stable for the life of a contact and are not
// reused until the 32-bit counter wraps, so a pointer capture keyed by id cannot match a different
// contact.
inline constexpr std::uint32_t kTouchPointerIdBase = 0x8000'0000U;

// Bounded contact table: a frame that reports more simultaneous contacts drops the surplus (and
// reports overflow) instead of allocating. The router's capture map is smaller; surplus contacts
// still dispatch, they just are not captured.
inline constexpr std::size_t kMaxTouchContacts = 16;

// Maps XI2 touch sequences (device id + touch id) onto stable normalized pointer events with
// `PointerKind::Touch`. The tracker keeps the last position per slot so move/up deltas are the
// change since that contact's previous event, never another contact's.
class TouchTracker {
public:
    // Fills `out` from the sample (logical pixels, already divided by the DPI scale) and assigns
    // the slot id on begin. `out.is_primary` marks the first contact of a sequence.
    [[nodiscard]] TouchResult feed(TouchPhase phase, std::uint32_t device_id,
                                   std::uint32_t touch_id, float x, float y, ui::InputEvent& out);

    // Cancels every live contact of a removed device (XI_HierarchyChanged) and returns how many
    // contacts ended. At most `capacity` cancellation events are written; the surplus contacts are
    // still ended, so a caller with a full buffer drops only the events.
    std::size_t cancel_device(std::uint32_t device_id, ui::InputEvent* out, std::size_t capacity);

    [[nodiscard]] bool live(std::uint32_t device_id, std::uint32_t touch_id) const;
    [[nodiscard]] std::size_t live_count() const;
    [[nodiscard]] bool overflowed() const {
        return overflowed_;
    }

    void reset();

private:
    struct Slot {
        std::uint32_t device_id = 0;
        std::uint32_t touch_id = 0;
        std::uint32_t pointer_id = 0;
        float x = 0.0F;
        float y = 0.0F;
        bool live = false;
        bool primary = false;
    };

    [[nodiscard]] Slot* find(std::uint32_t device_id, std::uint32_t touch_id);
    [[nodiscard]] const Slot* find(std::uint32_t device_id, std::uint32_t touch_id) const;
    [[nodiscard]] Slot* find_free();

    std::array<Slot, kMaxTouchContacts> slots_{};
    std::uint32_t next_pointer_id_ = kTouchPointerIdBase;
    bool overflowed_ = false;
};

// ---------------------------------------------------------------------------------------------
// Pen / tablet
// ---------------------------------------------------------------------------------------------

// A pen pressure axis is normalized into 0..1. Absent (or degenerate, min == max) pressure
// reports the same "pressed but unmeasured" 0.5 the core path uses while a button is down.
inline constexpr float kPenPressureDefault = 0.5F;
// Tilt is reported in normalized -1..1 where 1.0 is kPenTiltFullScaleDegrees from the surface
// normal. A device without tilt axes reports the neutral 0.0 default.
inline constexpr float kPenTiltDefault = 0.0F;
inline constexpr float kPenTiltFullScaleDegrees = 64.0F;

// XI2 has no tool class: an eraser reaches X as its own slave device and is named "eraser" by the
// driver (xf86-input-wacom/libinput). The name heuristic is the only portable signal, so a pen
// whose device name carries neither word is `Unknown` and keeps the pen mapping.
enum class PenTool : std::uint8_t { Unknown, Pen, Eraser };

// Everything the pump mirrors from one XI2 device event for a pen: the device's pressure/tilt axis
// values and ranges come from its XIValuatorClass records, the position and buttons from
// XIDeviceEvent.
struct PenSample {
    std::uint32_t device_id = 0;
    PenTool tool = PenTool::Pen;
    ui::InputEventType type = ui::InputEventType::PointerMove;
    // Value-initialized: Xlib defines a `None` macro, so the enumerators cannot be named here (the
    // pure header is also included after <X11/Xlib.h>). Both defaults are the `None` enumerator.
    ui::PointerButton button{};
    ui::Modifier modifiers{};
    float x = 0.0F;
    float y = 0.0F;
    float dx = 0.0F;
    float dy = 0.0F;
    // Raw pressure valuator value and its axis range from the device's XIValuatorClass.
    bool has_pressure = false;
    float raw_pressure = 0.0F;
    float pressure_min = 0.0F;
    float pressure_max = 1.0F;
    // Raw tilt valuator values in degrees; absent axes report kPenTiltDefault.
    bool has_tilt_x = false;
    bool has_tilt_y = false;
    float raw_tilt_x = 0.0F;
    float raw_tilt_y = 0.0F;
    // A pen hovers out of contact, where pressure is 0.
    bool in_contact = false;
};

// The framework models pen pressure (`ui::InputEvent::pressure`) and the device kind, but has no
// tilt or tool field: `tilt_x`/`tilt_y`/`tool` are reported here for callers that can use them and
// are dropped at the normalized-event boundary. Documented in docs/design/input.md.
struct PenValue {
    float pressure = kPenPressureDefault;
    float tilt_x = kPenTiltDefault;
    float tilt_y = kPenTiltDefault;
    PenTool tool = PenTool::Unknown;
};

// (raw - min) / (max - min) clamped to 0..1. A degenerate or non-finite range (no pressure axis)
// yields kPenPressureDefault; a non-finite raw value yields 0.
[[nodiscard]] float normalize_pen_pressure(float raw, float min, float max);

// Raw tilt in degrees scaled by kPenTiltFullScaleDegrees and clamped to -1..1; non-finite -> 0.
[[nodiscard]] float normalize_pen_tilt(float raw_degrees);

// Case-insensitive device-name heuristic: "eraser" wins over "pen"/"stylus"; other pointer devices
// (mouse, touchpad, touchscreen) are Unknown.
[[nodiscard]] PenTool pen_tool_from_device_name(std::string_view name);

// Fills `out` (kind Pen, position, button, pressure) and `value` (pressure, tilt, tool). Returns
// false only for an event type that is not a pen pointer event.
[[nodiscard]] bool make_pen_event(const PenSample& sample, ui::InputEvent& out, PenValue& value);

// ---------------------------------------------------------------------------------------------
// Per-device profile
// ---------------------------------------------------------------------------------------------

// Bound on the XI2 device table the pump keeps. Real desktops report well under a dozen pointer
// slaves; a larger hierarchy drops the surplus devices, which then keep the core-protocol path.
inline constexpr std::size_t kMaxXi2Devices = 32;

// What the pump knows about one XI2 slave device, assembled from XIQueryDevice class records. The
// valuator numbers and increments are opaque protocol values; the rules that consume them are the
// scroll accumulator, the touch tracker and the pen normalizers above.
struct DeviceProfile {
    std::uint32_t device_id = 0;
    bool touch_capable = false;
    bool scroll_capable = false;
    int vertical_valuator = -1;
    int horizontal_valuator = -1;
    ScrollAccumulator scroll{};
    int pressure_valuator = -1;
    float pressure_min = 0.0F;
    float pressure_max = 1.0F;
    int tilt_x_valuator = -1;
    int tilt_y_valuator = -1;
    PenTool tool = PenTool::Unknown;

    [[nodiscard]] bool is_pen() const {
        return tool != PenTool::Unknown;
    }
    [[nodiscard]] bool is_eraser() const {
        return tool == PenTool::Eraser;
    }
};

} // namespace tessera::hal::x11
