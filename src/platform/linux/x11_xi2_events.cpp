// SPDX-License-Identifier: MIT
//
// XI2 device-event translation for the X11 pump: motion (smooth-scroll valuators and pointer
// moves), buttons (notches and presses), touch sequences and pen/tablet samples. Split out of
// x11_xi2.cpp to keep both translation units small.

#include "x11_input_translation.hpp"
#include "x11_xi2.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace tessera::hal {
namespace {

// Same "button is down but the device reports no pressure" value the core path uses.
constexpr float kActivePressure = 0.5F;

float logical_coordinate(double physical, float scale) {
    return static_cast<float>(physical) / scale;
}

} // namespace

#if defined(TESSERA_X11_XI2)
bool X11Xi2Pump::handle_cookie(const XGenericEventCookie& cookie, float scale) {
    switch (cookie.evtype) {
    case XI_HierarchyChanged:
        handle_hierarchy();
        return true;
    case XI_TouchBegin:
    case XI_TouchUpdate:
    case XI_TouchEnd:
        if (cookie.data != nullptr) {
            handle_touch(*static_cast<const XIDeviceEvent*>(cookie.data), cookie.evtype, scale);
        }
        return true;
    case XI_TouchOwnership:
        // Ownership events only reach clients that select them and take a touch grab; this pump
        // does neither, so one arriving anyway changes nothing.
        return true;
    case XI_Motion:
    case XI_ButtonPress:
    case XI_ButtonRelease: {
        if (cookie.data == nullptr) {
            return true;
        }
        const auto& device = *static_cast<const XIDeviceEvent*>(cookie.data);
        if ((device.flags & XIPointerEmulated) != 0) {
            // The server emulates wheel buttons from valuators, valuators from wheel buttons and
            // pointer events from touch. The hardware event of each pair is the one without the
            // flag; dropping the emulated half is what keeps one notch to one event.
            return true;
        }
        const std::uint32_t source =
            static_cast<std::uint32_t>(device.sourceid != 0 ? device.sourceid : device.deviceid);
        x11::DeviceProfile* profile = find_device(source);
        if (profile == nullptr) {
            // A device that appeared after the table was built (or beyond its bound): keep the
            // core-shape mouse behavior instead of dropping the input.
            fallback_ = x11::DeviceProfile{};
            fallback_.device_id = source;
            if (cookie.evtype == XI_Motion) {
                handle_motion(device, fallback_, scale);
            } else {
                handle_button(device, fallback_, cookie.evtype == XI_ButtonPress, scale);
            }
            return true;
        }
        if (cookie.evtype == XI_Motion) {
            handle_motion(device, *profile, scale);
        } else {
            handle_button(device, *profile, cookie.evtype == XI_ButtonPress, scale);
        }
        return true;
    }
    default:
        return false;
    }
}

void X11Xi2Pump::handle_hierarchy() {
    // Cancel live contacts first: a device that left must not leave a captured pointer behind.
    std::array<ui::InputEvent, x11::kMaxTouchContacts> cancellations{};
    std::size_t written = 0;
    for (std::size_t index = 0; index < device_count_; ++index) {
        const std::size_t ended =
            touch_.cancel_device(devices_[index].device_id, cancellations.data() + written,
                                 cancellations.size() - written);
        written = std::min(written + ended, cancellations.size());
        if (written >= cancellations.size()) {
            break;
        }
    }
    for (std::size_t index = 0; index < written; ++index) {
        push(cancellations[index]);
    }
    touch_.reset(); // contacts of a device that is no longer in the table
    if (!rebuild_devices()) {
        device_count_ = 0;
    }
    if (has_touch_ && !select_events()) {
        // Re-selection failed (a protocol error): stop claiming XI2 instead of leaving the window
        // with neither the XI2 nor the core pointer path.
        enabled_ = false;
    }
}

bool X11Xi2Pump::valuator_value(const XIValuatorState& state, int number, double& out) const {
    if (number < 0 || state.mask == nullptr || state.values == nullptr) {
        return false;
    }
    if (number >= state.mask_len * 8) {
        return false;
    }
    if (!XIMaskIsSet(state.mask, number)) {
        return false;
    }
    // XIValuatorState::values is packed: only the valuators whose mask bit is set have a slot, in
    // ascending valuator order, so the slot index is the number of set bits below `number`.
    int index = 0;
    for (int bit = 0; bit < number; ++bit) {
        if (XIMaskIsSet(state.mask, bit)) {
            ++index;
        }
    }
    out = state.values[index];
    return true;
}

bool X11Xi2Pump::wheel_from_valuators(const XIDeviceEvent& device, x11::DeviceProfile& profile,
                                      float x, float y, ui::Modifier modifiers) {
    double raw_vertical = 0.0;
    double raw_horizontal = 0.0;
    const bool has_vertical =
        valuator_value(device.valuators, profile.vertical_valuator, raw_vertical);
    const bool has_horizontal =
        valuator_value(device.valuators, profile.horizontal_valuator, raw_horizontal);
    if (!has_vertical && !has_horizontal) {
        return false;
    }
    float dx = 0.0F;
    float dy = 0.0F;
    if (!profile.scroll.add(has_vertical ? static_cast<float>(raw_vertical) : 0.0F,
                            has_horizontal ? static_cast<float>(raw_horizontal) : 0.0F, dx, dy)) {
        return false;
    }
    ui::InputEvent wheel;
    wheel.type = ui::InputEventType::Wheel;
    wheel.x = x;
    wheel.y = y;
    wheel.dx = dx;
    wheel.dy = dy;
    wheel.modifiers = modifiers;
    wheel.is_primary = true;
    push(wheel);
    return true;
}

void X11Xi2Pump::handle_motion(const XIDeviceEvent& device, x11::DeviceProfile& profile,
                               float scale) {
    const float x = logical_coordinate(device.event_x, scale);
    const float y = logical_coordinate(device.event_y, scale);
    const ui::Modifier modifiers =
        x11::modifiers_from_state(static_cast<unsigned>(device.mods.effective));
    if (profile.is_pen()) {
        handle_pen_event(device, profile, ui::InputEventType::PointerMove, ui::PointerButton{},
                         buttons_down_ != 0, scale);
        return;
    }
    (void)wheel_from_valuators(device, profile, x, y, modifiers);
    float dx = 0.0F;
    float dy = 0.0F;
    track(x, y, dx, dy);
    ui::InputEvent move;
    move.type = ui::InputEventType::PointerMove;
    move.x = x;
    move.y = y;
    move.dx = dx;
    move.dy = dy;
    move.modifiers = modifiers;
    move.is_primary = true;
    move.pressure = buttons_down_ != 0 ? kActivePressure : 0.0F;
    push(move);
}

void X11Xi2Pump::handle_button(const XIDeviceEvent& device, x11::DeviceProfile& profile, bool press,
                               float scale) {
    const float x = logical_coordinate(device.event_x, scale);
    const float y = logical_coordinate(device.event_y, scale);
    const ui::Modifier modifiers =
        x11::modifiers_from_state(static_cast<unsigned>(device.mods.effective));
    const x11::ButtonAction action = x11::button_action(static_cast<unsigned>(device.detail));
    if (profile.is_pen()) {
        if (action.kind != x11::ButtonAction::Kind::Button) {
            return; // a pen has no wheel; a barrel/tip button is the left button
        }
        const std::uint32_t bit = 1U << static_cast<std::uint32_t>(action.button);
        if (press) {
            buttons_down_ |= bit;
            handle_pen_event(device, profile, ui::InputEventType::PointerDown, action.button, true,
                             scale);
        } else if ((buttons_down_ & bit) != 0) {
            handle_pen_event(device, profile, ui::InputEventType::PointerUp, action.button, false,
                             scale);
            buttons_down_ &= ~bit;
        }
        return;
    }
    if (action.kind == x11::ButtonAction::Kind::Wheel) {
        ui::InputEvent wheel;
        wheel.type = ui::InputEventType::Wheel;
        wheel.x = x;
        wheel.y = y;
        wheel.dx = action.dx;
        wheel.dy = action.dy;
        wheel.modifiers = modifiers;
        wheel.is_primary = true;
        push(wheel);
        return;
    }
    if (action.kind != x11::ButtonAction::Kind::Button) {
        return;
    }
    const std::uint32_t bit = 1U << static_cast<std::uint32_t>(action.button);
    if (press) {
        if ((buttons_down_ & bit) != 0) {
            return; // duplicate press this window already saw
        }
        buttons_down_ |= bit;
        float dx = 0.0F;
        float dy = 0.0F;
        track(x, y, dx, dy);
        ui::InputEvent down;
        down.type = ui::InputEventType::PointerDown;
        down.x = x;
        down.y = y;
        down.dx = dx;
        down.dy = dy;
        down.modifiers = modifiers;
        down.is_primary = true;
        down.button = action.button;
        down.pressure = kActivePressure;
        push(down);
        return;
    }
    if ((buttons_down_ & bit) == 0) {
        return; // a release for a press this window never saw
    }
    buttons_down_ &= ~bit;
    float dx = 0.0F;
    float dy = 0.0F;
    track(x, y, dx, dy);
    ui::InputEvent up;
    up.type = ui::InputEventType::PointerUp;
    up.x = x;
    up.y = y;
    up.dx = dx;
    up.dy = dy;
    up.modifiers = modifiers;
    up.is_primary = true;
    up.button = action.button;
    up.pressure = 0.0F;
    push(up);
}

void X11Xi2Pump::handle_pen_event(const XIDeviceEvent& device, x11::DeviceProfile& profile,
                                  ui::InputEventType type, ui::PointerButton button,
                                  bool in_contact, float scale) {
    const float x = logical_coordinate(device.event_x, scale);
    const float y = logical_coordinate(device.event_y, scale);
    float dx = 0.0F;
    float dy = 0.0F;
    track(x, y, dx, dy);
    x11::PenSample sample;
    sample.device_id = profile.device_id;
    sample.tool = profile.tool;
    sample.type = type;
    sample.button = button;
    sample.modifiers = x11::modifiers_from_state(static_cast<unsigned>(device.mods.effective));
    sample.x = x;
    sample.y = y;
    sample.dx = dx;
    sample.dy = dy;
    sample.in_contact = in_contact;
    double raw = 0.0;
    if (valuator_value(device.valuators, profile.pressure_valuator, raw)) {
        sample.has_pressure = true;
        sample.raw_pressure = static_cast<float>(raw);
        sample.pressure_min = profile.pressure_min;
        sample.pressure_max = profile.pressure_max;
    }
    if (valuator_value(device.valuators, profile.tilt_x_valuator, raw)) {
        sample.has_tilt_x = true;
        sample.raw_tilt_x = static_cast<float>(raw);
    }
    if (valuator_value(device.valuators, profile.tilt_y_valuator, raw)) {
        sample.has_tilt_y = true;
        sample.raw_tilt_y = static_cast<float>(raw);
    }
    ui::InputEvent event;
    x11::PenValue value;
    if (x11::make_pen_event(sample, event, value)) {
        push(event);
    }
}

void X11Xi2Pump::handle_touch(const XIDeviceEvent& device, int evtype, float scale) {
    if (!has_touch_) {
        return;
    }
    x11::TouchPhase phase = x11::TouchPhase::Update;
    switch (evtype) {
    case XI_TouchBegin:
        phase = x11::TouchPhase::Begin;
        break;
    case XI_TouchEnd:
        phase = x11::TouchPhase::End;
        break;
    default:
        break;
    }
    const std::uint32_t device_id =
        static_cast<std::uint32_t>(device.sourceid != 0 ? device.sourceid : device.deviceid);
    const std::uint32_t touch_id = static_cast<std::uint32_t>(device.detail);
    const float x = logical_coordinate(device.event_x, scale);
    const float y = logical_coordinate(device.event_y, scale);
    ui::InputEvent event;
    if (touch_.feed(phase, device_id, touch_id, x, y, event) == x11::TouchResult::Emitted) {
        push(event);
    }
}

#endif // TESSERA_X11_XI2

} // namespace tessera::hal
