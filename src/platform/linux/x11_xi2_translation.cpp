// SPDX-License-Identifier: MIT

#include "x11_xi2_translation.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace tessera::hal::x11 {

// ---------------------------------------------------------------------------------------------
// ScrollAccumulator
// ---------------------------------------------------------------------------------------------

void ScrollAccumulator::configure(Axis& axis, float increment) {
    axis.increment = increment;
    axis.enabled = std::isfinite(increment) && increment > 0.0F;
    if (!axis.enabled) {
        axis.pending = 0.0F;
    }
}

void ScrollAccumulator::set_vertical_increment(float increment) {
    configure(vertical_, increment);
}

void ScrollAccumulator::set_horizontal_increment(float increment) {
    configure(horizontal_, increment);
}

void ScrollAccumulator::reset() {
    vertical_.pending = 0.0F;
    horizontal_.pending = 0.0F;
}

bool ScrollAccumulator::accumulate(Axis& axis, float raw, float& out) {
    if (!axis.enabled || !std::isfinite(raw)) {
        return false;
    }
    axis.pending += raw / axis.increment;
    if (!std::isfinite(axis.pending)) {
        // A pathological delta (overflow to infinity) must not poison every later event.
        axis.pending = 0.0F;
        return false;
    }
    const float whole = std::trunc(axis.pending);
    if (whole == 0.0F) {
        return false;
    }
    axis.pending -= whole;
    out = whole;
    return true;
}

bool ScrollAccumulator::add(float raw_vertical, float raw_horizontal, float& out_dx,
                            float& out_dy) {
    out_dx = 0.0F;
    out_dy = 0.0F;
    const bool vertical = accumulate(vertical_, raw_vertical, out_dy);
    const bool horizontal = accumulate(horizontal_, raw_horizontal, out_dx);
    return vertical || horizontal;
}

// ---------------------------------------------------------------------------------------------
// TouchTracker
// ---------------------------------------------------------------------------------------------

TouchTracker::Slot* TouchTracker::find(std::uint32_t device_id, std::uint32_t touch_id) {
    for (Slot& slot : slots_) {
        if (slot.live && slot.device_id == device_id && slot.touch_id == touch_id) {
            return &slot;
        }
    }
    return nullptr;
}

const TouchTracker::Slot* TouchTracker::find(std::uint32_t device_id,
                                             std::uint32_t touch_id) const {
    for (const Slot& slot : slots_) {
        if (slot.live && slot.device_id == device_id && slot.touch_id == touch_id) {
            return &slot;
        }
    }
    return nullptr;
}

TouchTracker::Slot* TouchTracker::find_free() {
    for (Slot& slot : slots_) {
        if (!slot.live) {
            return &slot;
        }
    }
    return nullptr;
}

TouchResult TouchTracker::feed(TouchPhase phase, std::uint32_t device_id, std::uint32_t touch_id,
                               float x, float y, ui::InputEvent& out) {
    out = ui::InputEvent{};
    if (phase == TouchPhase::Begin) {
        if (find(device_id, touch_id) != nullptr) {
            return TouchResult::Ignored; // duplicate begin for a live contact
        }
        Slot* slot = find_free();
        if (slot == nullptr) {
            overflowed_ = true;
            return TouchResult::Ignored;
        }
        const std::uint32_t pointer_id = next_pointer_id_++;
        if (next_pointer_id_ == 0U) {
            next_pointer_id_ = kTouchPointerIdBase; // skip 0, the mouse/pen default id
        }
        const bool primary = live_count() == 0;
        slot->device_id = device_id;
        slot->touch_id = touch_id;
        slot->pointer_id = pointer_id;
        slot->x = x;
        slot->y = y;
        slot->live = true;
        slot->primary = primary;

        out.type = ui::InputEventType::PointerDown;
        out.pointer_id = pointer_id;
        out.pointer_kind = ui::PointerKind::Touch;
        out.x = x;
        out.y = y;
        out.is_primary = primary;
        return TouchResult::Emitted;
    }

    Slot* slot = find(device_id, touch_id);
    if (slot == nullptr) {
        return TouchResult::Ignored; // update/end/cancel for a contact that never began
    }
    const std::uint32_t pointer_id = slot->pointer_id;
    const float delta_x = x - slot->x;
    const float delta_y = y - slot->y;
    out.pointer_id = pointer_id;
    out.pointer_kind = ui::PointerKind::Touch;
    out.is_primary = slot->primary;
    switch (phase) {
    case TouchPhase::Update:
        out.type = ui::InputEventType::PointerMove;
        out.x = x;
        out.y = y;
        out.dx = delta_x;
        out.dy = delta_y;
        slot->x = x;
        slot->y = y;
        return TouchResult::Emitted;
    case TouchPhase::End:
        out.type = ui::InputEventType::PointerUp;
        out.x = x;
        out.y = y;
        out.dx = delta_x;
        out.dy = delta_y;
        break;
    case TouchPhase::Cancel:
        // A cancel is not a move: report the last known position and no delta.
        out.type = ui::InputEventType::PointerCancel;
        out.x = slot->x;
        out.y = slot->y;
        break;
    case TouchPhase::Begin:
        break; // unreachable: Begin returned above
    }
    slot->live = false;
    slot->device_id = 0;
    slot->touch_id = 0;
    slot->pointer_id = 0;
    slot->primary = false;
    return TouchResult::Emitted;
}

std::size_t TouchTracker::cancel_device(std::uint32_t device_id, ui::InputEvent* out,
                                        std::size_t capacity) {
    if (out == nullptr) {
        return 0;
    }
    std::size_t written = 0;
    std::size_t live = 0;
    for (Slot& slot : slots_) {
        if (!slot.live || slot.device_id != device_id) {
            continue;
        }
        ++live;
        if (written < capacity) {
            ui::InputEvent event;
            event.type = ui::InputEventType::PointerCancel;
            event.pointer_id = slot.pointer_id;
            event.pointer_kind = ui::PointerKind::Touch;
            event.x = slot.x;
            event.y = slot.y;
            event.is_primary = slot.primary;
            out[written] = event;
            ++written;
        }
        slot.live = false;
        slot.device_id = 0;
        slot.touch_id = 0;
        slot.pointer_id = 0;
        slot.primary = false;
    }
    // Return the number of contacts that ended, even when fewer fit the caller's buffer, so the
    // caller can report the drop.
    return live;
}

bool TouchTracker::live(std::uint32_t device_id, std::uint32_t touch_id) const {
    return find(device_id, touch_id) != nullptr;
}

std::size_t TouchTracker::live_count() const {
    std::size_t count = 0;
    for (const Slot& slot : slots_) {
        if (slot.live) {
            ++count;
        }
    }
    return count;
}

void TouchTracker::reset() {
    slots_ = {};
    next_pointer_id_ = kTouchPointerIdBase;
    overflowed_ = false;
}

// ---------------------------------------------------------------------------------------------
// Pen
// ---------------------------------------------------------------------------------------------

float normalize_pen_pressure(float raw, float min, float max) {
    if (!std::isfinite(raw)) {
        return 0.0F;
    }
    if (!std::isfinite(min) || !std::isfinite(max) || max <= min) {
        return kPenPressureDefault;
    }
    return std::clamp((raw - min) / (max - min), 0.0F, 1.0F);
}

float normalize_pen_tilt(float raw_degrees) {
    if (!std::isfinite(raw_degrees)) {
        return kPenTiltDefault;
    }
    return std::clamp(raw_degrees / kPenTiltFullScaleDegrees, -1.0F, 1.0F);
}

namespace {

char ascii_lower(char value) {
    return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
}

bool contains_ignore_case(std::string_view haystack, std::string_view needle) {
    if (needle.empty() || needle.size() > haystack.size()) {
        return false;
    }
    for (std::size_t start = 0; start + needle.size() <= haystack.size(); ++start) {
        bool match = true;
        for (std::size_t index = 0; index < needle.size(); ++index) {
            if (ascii_lower(haystack[start + index]) != ascii_lower(needle[index])) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

} // namespace

PenTool pen_tool_from_device_name(std::string_view name) {
    // An eraser is a separate slave device whose name carries both "pen" and "eraser"; the eraser
    // word has to win, otherwise the tool would be indistinguishable from its pen.
    if (contains_ignore_case(name, "eraser")) {
        return PenTool::Eraser;
    }
    if (contains_ignore_case(name, "pen") || contains_ignore_case(name, "stylus") ||
        contains_ignore_case(name, "tablet")) {
        return PenTool::Pen;
    }
    return PenTool::Unknown;
}

bool make_pen_event(const PenSample& sample, ui::InputEvent& out, PenValue& value) {
    switch (sample.type) {
    case ui::InputEventType::PointerMove:
    case ui::InputEventType::PointerDown:
    case ui::InputEventType::PointerUp:
    case ui::InputEventType::PointerCancel:
        break;
    default:
        return false;
    }
    value.tool = sample.tool;
    value.pressure =
        sample.has_pressure
            ? normalize_pen_pressure(sample.raw_pressure, sample.pressure_min, sample.pressure_max)
            : kPenPressureDefault;
    value.tilt_x = sample.has_tilt_x ? normalize_pen_tilt(sample.raw_tilt_x) : kPenTiltDefault;
    value.tilt_y = sample.has_tilt_y ? normalize_pen_tilt(sample.raw_tilt_y) : kPenTiltDefault;

    out = ui::InputEvent{};
    out.type = sample.type;
    out.device_id = sample.device_id;
    out.pointer_kind = ui::PointerKind::Pen;
    out.x = sample.x;
    out.y = sample.y;
    out.dx = sample.dx;
    out.dy = sample.dy;
    out.button = sample.button;
    out.modifiers = sample.modifiers;
    out.is_primary = true;
    // A hovering pen is out of contact: it still moves, but reports no pressure.
    out.pressure = sample.in_contact ? value.pressure : 0.0F;
    return true;
}

} // namespace tessera::hal::x11
