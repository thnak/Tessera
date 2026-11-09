// SPDX-License-Identifier: MIT

#include "win32_pointer_translation.hpp"

#include <algorithm>

namespace tessera::hal::win32 {
namespace {

constexpr std::uint32_t kPenPressureMax = 1024;
constexpr std::uint32_t kPointerIdMask = 0x7FFF'FFFFu;

ui::InputEvent base_event(ui::InputEventType type, float x, float y, float dpi_scale) {
    ui::InputEvent event;
    event.type = type;
    const float scale = dpi_scale > 0.0f ? dpi_scale : 1.0f;
    event.x = x / scale;
    event.y = y / scale;
    return event;
}

} // namespace

ui::PointerKind pointer_kind_of(PointerType type) {
    switch (type) {
    case PointerType::Mouse:
        return ui::PointerKind::Mouse;
    case PointerType::Touch:
        return ui::PointerKind::Touch;
    case PointerType::Pen:
        return ui::PointerKind::Pen;
    case PointerType::Unknown:
        break;
    }
    // Legacy messages and future pointer types keep the mouse behavior; input is never dropped.
    return ui::PointerKind::Mouse;
}

std::uint32_t touch_pointer_id(std::uint32_t touch_id) {
    return kTouchPointerIdBase | (touch_id & kPointerIdMask);
}

float normalize_pen_pressure(std::uint32_t raw_pressure) {
    return static_cast<float>(std::min(raw_pressure, kPenPressureMax)) /
           static_cast<float>(kPenPressureMax);
}

bool make_pointer_event(const PointerSample& sample, float dpi_scale, ui::InputEvent& out) {
    // Touch contacts have no pre-contact position: never synthesize a hover move for them. A
    // cancel is still delivered so the pointer's capture/active state can be cleared.
    if (sample.type == PointerType::Touch && !sample.in_contact &&
        sample.message != PointerMessage::Cancel) {
        return false;
    }

    ui::InputEventType type = ui::InputEventType::PointerMove;
    switch (sample.message) {
    case PointerMessage::Down:
        type = ui::InputEventType::PointerDown;
        break;
    case PointerMessage::Update:
        type = ui::InputEventType::PointerMove;
        break;
    case PointerMessage::Up:
        type = ui::InputEventType::PointerUp;
        break;
    case PointerMessage::Cancel:
        type = ui::InputEventType::PointerCancel;
        break;
    }

    out = base_event(type, sample.client_x, sample.client_y, dpi_scale);
    out.pointer_id = sample.pointer_id;
    out.pointer_kind = pointer_kind_of(sample.type);
    out.is_primary = sample.primary;
    out.button = sample.button;
    out.pressure = sample.type == PointerType::Pen ? normalize_pen_pressure(sample.pressure) : 0.0f;
    return true;
}

bool make_touch_event(const TouchSample& sample, float dpi_scale, ui::InputEvent& out) {
    ui::InputEventType type = ui::InputEventType::PointerMove;
    if (sample.up) {
        type = ui::InputEventType::PointerUp;
    } else if (sample.down) {
        type = ui::InputEventType::PointerDown;
    } else if (!sample.move) {
        // No contact flag: nothing to emit (in particular no synthesized hover).
        return false;
    }

    out = base_event(type, sample.client_x, sample.client_y, dpi_scale);
    out.pointer_id = touch_pointer_id(sample.touch_id);
    out.pointer_kind = ui::PointerKind::Touch;
    out.is_primary = sample.primary;
    out.pressure = 0.0f;
    out.button = (sample.down || sample.up) ? ui::PointerButton::Left : ui::PointerButton::None;
    return true;
}

} // namespace tessera::hal::win32
