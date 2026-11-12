// SPDX-License-Identifier: MIT
//
// Pure XInput2 mapping rules (smooth-scroll valuators, touch contacts, pen samples) and the XIM
// pre-edit composition state machine: platform independent, no display, no input method, no clock.

#include "x11_ime_composition.hpp"
#include "x11_xi2_test_support.hpp"
#include "x11_xi2_translation.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace x11 = tessera::hal::x11;
namespace ui = tessera::ui;

using x11_test::expect;
using x11_test::same_event;

// --- 1. Valuator scroll ----------------------------------------------------------------------

bool scroll_axis_signs() {
    x11::ScrollAccumulator scroll;
    scroll.set_vertical_increment(1.0F);
    scroll.set_horizontal_increment(120.0F);
    float dx = 0.0F;
    float dy = 0.0F;
    bool ok =
        expect(scroll.add(1.0F, 0.0F, dx, dy), "one notch down is due") &&
        expect(dx == 0.0F && dy == 1.0F, "positive vertical delta is a down notch (web dy)") &&
        expect(scroll.add(-2.0F, 0.0F, dx, dy), "two notches up are due") &&
        expect(dx == 0.0F && dy == -2.0F, "negative vertical delta is an up notch") &&
        expect(scroll.add(0.0F, 120.0F, dx, dy), "one horizontal notch is due") &&
        expect(dx == 1.0F && dy == 0.0F, "positive horizontal delta is a right notch") &&
        expect(scroll.add(0.0F, -240.0F, dx, dy), "two left notches are due") &&
        expect(dx == -2.0F && dy == 0.0F, "negative horizontal delta is a left notch");
    return ok;
}

bool scroll_fractional_accumulation() {
    x11::ScrollAccumulator scroll;
    scroll.set_vertical_increment(120.0F);
    float dx = 0.0F;
    float dy = 0.0F;
    // Two raw half-notches: no event, then exactly one whole notch (no drift, no double emit),
    // keeping the remainder for the next sample.
    bool ok = expect(!scroll.add(60.0F, 0.0F, dx, dy), "half a notch emits nothing") &&
              expect(scroll.add(60.0F, 0.0F, dx, dy), "the second half completes a notch") &&
              expect(dy == 1.0F, "the completed notch is exactly one unit") &&
              expect(!scroll.add(60.0F, 0.0F, dx, dy), "the remainder is below a notch") &&
              expect(scroll.add(60.0F, 0.0F, dx, dy), "the next pair completes a notch") &&
              expect(dy == 1.0F, "no accumulated drift after several notches");
    // A smooth axis with a unit increment: quarters add up to exactly one, never two.
    scroll.set_vertical_increment(1.0F);
    scroll.reset();
    ok = ok &&
         expect(!scroll.add(0.25F, 0.0F, dx, dy) && !scroll.add(0.25F, 0.0F, dx, dy) &&
                    !scroll.add(0.25F, 0.0F, dx, dy),
                "three quarters emit nothing") &&
         expect(scroll.add(0.25F, 0.0F, dx, dy) && dy == 1.0F, "the fourth quarter emits one") &&
         expect(!scroll.add(0.0F, 0.0F, dx, dy), "a zero delta emits nothing");
    // Reset drops a pending sub-unit remainder instead of carrying it into the next gesture.
    ok = ok && expect(!scroll.add(0.5F, 0.0F, dx, dy), "half a notch is pending");
    scroll.reset();
    ok = ok && expect(!scroll.add(0.5F, 0.0F, dx, dy), "reset dropped the pending half");
    return ok;
}

bool scroll_disabled_and_hostile_values() {
    float dx = 1.0F;
    float dy = 1.0F;
    x11::ScrollAccumulator scroll;
    // A missing ScrollClass, a zero increment or a nonsense increment disables the axis: deltas are
    // dropped rather than divided by zero.
    scroll.set_vertical_increment(0.0F);
    scroll.set_horizontal_increment(-4.0F);
    bool ok = expect(!scroll.vertical_enabled() && !scroll.horizontal_enabled(),
                     "zero and negative increments disable their axis") &&
              expect(!scroll.add(3.0F, 3.0F, dx, dy), "a disabled axis never emits") &&
              expect(dx == 0.0F && dy == 0.0F, "disabled axes report zero deltas");
    scroll.set_vertical_increment(std::nanf(""));
    scroll.set_horizontal_increment(INFINITY);
    ok = ok && expect(!scroll.add(1.0F, 1.0F, dx, dy), "non-finite increments disable the axis");
    scroll.set_vertical_increment(1.0F);
    scroll.set_horizontal_increment(1.0F);
    ok = ok && expect(!scroll.add(std::nanf(""), 0.0F, dx, dy), "a NaN delta is ignored") &&
         expect(!scroll.add(0.0F, INFINITY, dx, dy), "an infinite delta is ignored") &&
         expect(scroll.add(0.0F, 0.0F, dx, dy) == false, "nothing pending emits nothing") &&
         expect(scroll.add(1.0F, 0.0F, dx, dy) && dy == 1.0F,
                "the axis still works after hostile input");
    // Both axes can complete on the same sample: one diagonal wheel event.
    ok = ok && expect(scroll.add(1.0F, 1.0F, dx, dy), "a diagonal sample emits one event") &&
         expect(dx == 1.0F && dy == 1.0F, "both axes carry their delta");
    return ok;
}

// --- 2. Touch --------------------------------------------------------------------------------

bool touch_sequence() {
    x11::TouchTracker touch;
    ui::InputEvent event;
    bool ok =
        expect(touch.feed(x11::TouchPhase::Begin, 7, 3, 10.0F, 20.0F, event) ==
                   x11::TouchResult::Emitted,
               "a begin emits") &&
        expect(event.type == ui::InputEventType::PointerDown, "a begin is PointerDown") &&
        expect(event.pointer_kind == ui::PointerKind::Touch, "touch kind is Touch") &&
        expect(event.pointer_id >= x11::kTouchPointerIdBase, "touch ids are in the touch range") &&
        expect(event.is_primary, "the first contact is primary") &&
        expect(event.x == 10.0F && event.y == 20.0F, "the begin carries the position") &&
        expect(event.dx == 0.0F && event.dy == 0.0F, "a begin has no delta");
    const std::uint32_t first = event.pointer_id;
    ok = ok &&
         expect(touch.feed(x11::TouchPhase::Update, 7, 3, 15.0F, 25.0F, event) ==
                    x11::TouchResult::Emitted,
                "an update emits") &&
         expect(event.type == ui::InputEventType::PointerMove, "an update is PointerMove") &&
         expect(event.pointer_id == first, "the update keeps the contact id") &&
         expect(event.dx == 5.0F && event.dy == 5.0F, "the update delta is per contact") &&
         expect(touch.feed(x11::TouchPhase::End, 7, 3, 18.0F, 28.0F, event) ==
                    x11::TouchResult::Emitted,
                "an end emits") &&
         expect(event.type == ui::InputEventType::PointerUp, "an end is PointerUp") &&
         expect(event.dx == 3.0F && event.dy == 3.0F, "the end carries the last delta") &&
         expect(touch.feed(x11::TouchPhase::Update, 7, 3, 19.0F, 29.0F, event) ==
                    x11::TouchResult::Ignored,
                "an update after end is ignored") &&
         expect(touch.feed(x11::TouchPhase::End, 7, 3, 20.0F, 30.0F, event) ==
                    x11::TouchResult::Ignored,
                "a repeated end is ignored") &&
         expect(touch.feed(x11::TouchPhase::Cancel, 9, 1, 0.0F, 0.0F, event) ==
                    x11::TouchResult::Ignored,
                "a cancel without a begin is ignored") &&
         expect(touch.live_count() == 0, "no contact is live after end");
    return ok;
}

bool touch_interleaved_slots() {
    x11::TouchTracker touch;
    ui::InputEvent event;
    bool ok = expect(touch.feed(x11::TouchPhase::Begin, 4, 0, 1.0F, 1.0F, event) ==
                         x11::TouchResult::Emitted,
                     "first slot begins");
    const std::uint32_t first = event.pointer_id;
    ok = ok &&
         expect(touch.feed(x11::TouchPhase::Begin, 4, 1, 100.0F, 100.0F, event) ==
                    x11::TouchResult::Emitted,
                "second slot begins") &&
         expect(!event.is_primary, "the second contact is not primary");
    const std::uint32_t second = event.pointer_id;
    ok = ok && expect(first != second, "interleaved contacts have distinct ids") &&
         expect(touch.feed(x11::TouchPhase::Update, 4, 0, 2.0F, 3.0F, event) ==
                        x11::TouchResult::Emitted &&
                    event.pointer_id == first && event.dx == 1.0F && event.dy == 2.0F,
                "slot 0 keeps its own delta") &&
         expect(touch.feed(x11::TouchPhase::Update, 4, 1, 90.0F, 104.0F, event) ==
                        x11::TouchResult::Emitted &&
                    event.pointer_id == second && event.dx == -10.0F && event.dy == 4.0F,
                "slot 1 keeps its own delta") &&
         expect(touch.live(4, 0) && touch.live(4, 1) && !touch.live(4, 2),
                "liveness is per (device, touch)") &&
         expect(touch.feed(x11::TouchPhase::End, 4, 0, 2.0F, 3.0F, event) ==
                        x11::TouchResult::Emitted &&
                    event.pointer_id == first,
                "ending one slot reports that slot's id") &&
         expect(touch.live(4, 1) && !touch.live(4, 0), "the other slot stays live") &&
         expect(touch.feed(x11::TouchPhase::Cancel, 4, 1, 0.0F, 0.0F, event) ==
                        x11::TouchResult::Emitted &&
                    event.type == ui::InputEventType::PointerCancel && event.pointer_id == second,
                "cancel reports the cancelled contact") &&
         expect(touch.live_count() == 0, "both slots ended");
    // A duplicate begin for a live contact does not allocate a second slot.
    ok = ok &&
         expect(touch.feed(x11::TouchPhase::Begin, 4, 5, 0.0F, 0.0F, event) ==
                    x11::TouchResult::Emitted,
                "slot five begins") &&
         expect(touch.feed(x11::TouchPhase::Begin, 4, 5, 0.0F, 0.0F, event) ==
                    x11::TouchResult::Ignored,
                "a duplicate begin is ignored") &&
         expect(touch.live_count() == 1, "the duplicate did not add a contact");
    return ok;
}

bool touch_bounds_and_device_removal() {
    x11::TouchTracker touch;
    ui::InputEvent event;
    bool ok = true;
    for (std::size_t index = 0; index < x11::kMaxTouchContacts; ++index) {
        ok = expect(touch.feed(x11::TouchPhase::Begin, 2, static_cast<std::uint32_t>(index), 0.0F,
                               0.0F, event) == x11::TouchResult::Emitted,
                    "contacts fill the bounded table") &&
             ok;
    }
    ok = ok &&
         expect(touch.feed(x11::TouchPhase::Begin, 2, 999, 0.0F, 0.0F, event) ==
                    x11::TouchResult::Ignored,
                "a contact beyond the table is dropped") &&
         expect(touch.overflowed(), "the drop is reported as overflow") &&
         expect(touch.live_count() == x11::kMaxTouchContacts, "the table stayed bounded");
    // Removing a device cancels its live contacts, and only them.
    ok = ok && expect(touch.feed(x11::TouchPhase::Begin, 3, 0, 0.0F, 0.0F, event) ==
                          x11::TouchResult::Ignored,
                      "a contact on another device is dropped while the table is full");
    std::vector<ui::InputEvent> cancellations(x11::kMaxTouchContacts + 4);
    const std::size_t ended = touch.cancel_device(2, cancellations.data(), cancellations.size());
    ok = ok &&
         expect(ended == x11::kMaxTouchContacts, "every contact of the removed device ends") &&
         expect(cancellations[0].type == ui::InputEventType::PointerCancel,
                "a device removal emits cancels") &&
         expect(touch.live_count() == 0, "no contact survives the removal");
    // The freed slots are reusable, and a device-scoped cancel leaves other devices alone.
    ok = ok && expect(touch.feed(x11::TouchPhase::Begin, 3, 0, 5.0F, 6.0F, event) ==
                          x11::TouchResult::Emitted,
                      "a contact begins after the removal");
    const std::uint32_t other = event.pointer_id;
    ok = ok &&
         expect(touch.feed(x11::TouchPhase::Begin, 4, 0, 0.0F, 0.0F, event) ==
                    x11::TouchResult::Emitted,
                "a second device begins") &&
         expect(touch.cancel_device(4, cancellations.data(), cancellations.size()) == 1,
                "only the removed device's contact ends") &&
         expect(touch.live_count() == 1 && touch.live(3, 0), "the other device is untouched") &&
         expect(touch.feed(x11::TouchPhase::Update, 3, 0, 6.0F, 8.0F, event) ==
                        x11::TouchResult::Emitted &&
                    event.pointer_id == other && event.dx == 1.0F && event.dy == 2.0F,
                "the surviving contact still reports its id and deltas");
    touch.reset();
    ok = ok && expect(touch.live_count() == 0 && !touch.overflowed(), "reset clears the table");
    return ok;
}

// --- 3. Pen ----------------------------------------------------------------------------------

bool pen_normalization() {
    bool ok =
        expect(x11::normalize_pen_pressure(0.0F, 0.0F, 2048.0F) == 0.0F, "no pressure") &&
        expect(x11::normalize_pen_pressure(1024.0F, 0.0F, 2048.0F) == 0.5F, "half pressure") &&
        expect(x11::normalize_pen_pressure(4096.0F, 0.0F, 2048.0F) == 1.0F,
               "pressure above the axis maximum clamps to 1") &&
        expect(x11::normalize_pen_pressure(-10.0F, 0.0F, 2048.0F) == 0.0F,
               "pressure below the axis minimum clamps to 0") &&
        expect(x11::normalize_pen_pressure(1024.0F, 0.0F, 0.0F) == x11::kPenPressureDefault,
               "a degenerate axis reports the documented default") &&
        expect(x11::normalize_pen_pressure(std::nanf(""), 0.0F, 2048.0F) == 0.0F,
               "a non-finite pressure is zero") &&
        expect(x11::normalize_pen_tilt(0.0F) == x11::kPenTiltDefault, "a flat pen has no tilt") &&
        expect(x11::normalize_pen_tilt(32.0F) == 0.5F, "tilt scales by the full-scale angle") &&
        expect(x11::normalize_pen_tilt(-128.0F) == -1.0F, "tilt clamps at -1") &&
        expect(x11::normalize_pen_tilt(128.0F) == 1.0F, "tilt clamps at 1") &&
        expect(x11::normalize_pen_tilt(INFINITY) == x11::kPenTiltDefault,
               "a non-finite tilt is the neutral default");
    // Tool classification is a device-name heuristic; the eraser word has to win over "pen".
    ok = ok &&
         expect(x11::pen_tool_from_device_name("Wacom Intuos Pen stylus") == x11::PenTool::Pen,
                "a pen device is a pen") &&
         expect(x11::pen_tool_from_device_name("Wacom Intuos Pen ERASER") == x11::PenTool::Eraser,
                "an eraser device is an eraser") &&
         expect(x11::pen_tool_from_device_name("Logitech USB Mouse") == x11::PenTool::Unknown,
                "a mouse is not a pen") &&
         expect(x11::pen_tool_from_device_name("AT Translated Set 2 keyboard") ==
                    x11::PenTool::Unknown,
                "a keyboard is not a pen") &&
         expect(x11::pen_tool_from_device_name("") == x11::PenTool::Unknown,
                "an unnamed device is unknown");
    return ok;
}

bool pen_events() {
    x11::PenSample sample;
    sample.device_id = 11;
    sample.tool = x11::PenTool::Pen;
    sample.type = ui::InputEventType::PointerDown;
    sample.button = ui::PointerButton::Left;
    sample.x = 40.0F;
    sample.y = 50.0F;
    sample.dx = 2.0F;
    sample.dy = 3.0F;
    sample.has_pressure = true;
    sample.raw_pressure = 1536.0F;
    sample.pressure_min = 0.0F;
    sample.pressure_max = 2048.0F;
    sample.in_contact = true;
    ui::InputEvent event;
    x11::PenValue value;
    bool ok = expect(x11::make_pen_event(sample, event, value), "a pen down maps") &&
              expect(event.type == ui::InputEventType::PointerDown, "the type is forwarded") &&
              expect(event.pointer_kind == ui::PointerKind::Pen, "the kind is Pen") &&
              expect(event.pressure == 0.75F, "pressure is normalized and forwarded") &&
              expect(event.x == 40.0F && event.y == 50.0F && event.dx == 2.0F && event.dy == 3.0F,
                     "position and delta are forwarded") &&
              expect(event.button == ui::PointerButton::Left, "the button is forwarded") &&
              expect(event.device_id == 11, "the device id is forwarded") &&
              expect(value.tilt_x == x11::kPenTiltDefault && value.tilt_y == x11::kPenTiltDefault,
                     "a device without tilt reports the documented neutral default") &&
              expect(value.tool == x11::PenTool::Pen, "the tool is reported to the caller");
    // No pressure axis at all: the documented "pressed but unmeasured" default, not 0.
    sample.has_pressure = false;
    sample.in_contact = true;
    ok = ok && expect(x11::make_pen_event(sample, event, value) &&
                          value.pressure == x11::kPenPressureDefault && event.pressure == 0.5F,
                      "a pen without a pressure axis reports the default pressure");
    // Out of contact (hovering): the move still happens but carries no pressure.
    sample.has_pressure = true;
    sample.type = ui::InputEventType::PointerMove;
    sample.in_contact = false;
    ok = ok && expect(x11::make_pen_event(sample, event, value) && event.pressure == 0.0F,
                      "a hovering pen reports no pressure");
    // Tilt is modelled by the pure layer and dropped from the normalized event, which has no tilt
    // field; the eraser tool is likewise kept in PenValue only.
    sample.in_contact = true;
    sample.has_tilt_x = true;
    sample.has_tilt_y = true;
    sample.raw_tilt_x = 64.0F;
    sample.raw_tilt_y = -32.0F;
    sample.tool = x11::PenTool::Eraser;
    ok = ok &&
         expect(x11::make_pen_event(sample, event, value), "an eraser maps to a pointer event") &&
         expect(value.tilt_x == 1.0F && value.tilt_y == -0.5F, "tilt is normalized") &&
         expect(value.tool == x11::PenTool::Eraser, "the eraser is distinguishable") &&
         expect(event.pointer_kind == ui::PointerKind::Pen &&
                    event.button == ui::PointerButton::Left,
                "an eraser keeps the pen pointer shape (no eraser field exists)") &&
         expect(event.text_size == 0 && event.key == 0, "no unrelated payload is set");
    sample.type = ui::InputEventType::KeyDown;
    ok =
        ok && expect(!x11::make_pen_event(sample, event, value), "a non-pointer event is rejected");
    return ok;
}
// --- 5. Determinism --------------------------------------------------------------------------

std::vector<ui::InputEvent> replay_script() {
    std::vector<ui::InputEvent> events;
    x11::ScrollAccumulator scroll;
    x11::TouchTracker touch;
    x11::CompositionState composition;
    scroll.set_vertical_increment(120.0F);
    scroll.set_horizontal_increment(40.0F);
    ui::InputEvent event;
    float dx = 0.0F;
    float dy = 0.0F;
    const std::array<float, 5> raw_vertical{40.0F, 80.0F, 0.0F, -120.0F, 0.0F};
    for (float raw : raw_vertical) {
        if (scroll.add(raw, raw / 2.0F, dx, dy)) {
            event = ui::InputEvent{};
            event.type = ui::InputEventType::Wheel;
            event.dx = dx;
            event.dy = dy;
            events.push_back(event);
        }
    }
    for (const x11::TouchPhase phase : {x11::TouchPhase::Begin, x11::TouchPhase::Update,
                                        x11::TouchPhase::Update, x11::TouchPhase::End}) {
        if (touch.feed(phase, 5, 2, static_cast<float>(phase == x11::TouchPhase::Begin ? 1 : 2),
                       3.0F, event) == x11::TouchResult::Emitted) {
            events.push_back(event);
        }
    }
    x11::PenSample pen;
    pen.device_id = 3;
    pen.type = ui::InputEventType::PointerDown;
    pen.button = ui::PointerButton::Left;
    pen.has_pressure = true;
    pen.raw_pressure = 512.0F;
    pen.pressure_min = 0.0F;
    pen.pressure_max = 1024.0F;
    pen.in_contact = true;
    x11::PenValue value;
    if (x11::make_pen_event(pen, event, value)) {
        events.push_back(event);
    }
    for (const x11::CompositionOutput& output :
         {composition.start("ab", 1), composition.update("abc", 2), composition.commit()}) {
        for (std::size_t index = 0; index < output.count; ++index) {
            events.push_back(output.events[index]);
        }
    }
    return events;
}

bool determinism() {
    const std::vector<ui::InputEvent> first = replay_script();
    const std::vector<ui::InputEvent> second = replay_script();
    bool ok = expect(first.size() == second.size(), "the replay produces the same event count") &&
              expect(!first.empty(), "the replay produced events");
    const std::size_t shared = first.size() < second.size() ? first.size() : second.size();
    for (std::size_t index = 0; index < shared; ++index) {
        ok = same_event(first[index], second[index], "replayed event") && ok;
    }
    if (!ok) {
        return false;
    }
    // The scripted sequence also pins the expected shape: two wheel events (the half-notch samples
    // accumulate, the zero sample is a no-op), the touch family, the pen, and the composition.
    return expect(first.size() == 11, "the script emits eleven events") &&
           expect(first[0].type == ui::InputEventType::Wheel, "the script starts with a wheel") &&
           expect(first[1].type == ui::InputEventType::Wheel,
                  "the accumulated notch emits a wheel") &&
           expect(first[2].type == ui::InputEventType::PointerDown, "then a touch begins") &&
           expect(first[5].type == ui::InputEventType::PointerUp, "then the touch ends") &&
           expect(first[6].pointer_kind == ui::PointerKind::Pen, "then the pen") &&
           expect(first[7].type == ui::InputEventType::CompositionStart, "then the composition") &&
           expect(first[9].type == ui::InputEventType::EditText, "then the commit") &&
           expect(first[10].type == ui::InputEventType::CompositionEnd, "then the end");
}

} // namespace

int main() {
    if (!scroll_axis_signs() || !scroll_fractional_accumulation() ||
        !scroll_disabled_and_hostile_values()) {
        return 1;
    }
    if (!touch_sequence() || !touch_interleaved_slots() || !touch_bounds_and_device_removal()) {
        return 2;
    }
    if (!pen_normalization() || !pen_events()) {
        return 3;
    }
    if (!ime_composition_suite() || !ime_clamping_suite()) {
        return 4;
    }
    if (!determinism()) {
        return 5;
    }
    std::cout << "x11_xi2_translation: ok\n";
    return 0;
}
