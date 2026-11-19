// SPDX-License-Identifier: MIT
//
// System-button presses and their named scripts for the synthetic injector
// (docs/specs/studio-device-profiles-v1.md sections 2 and 4, issue #445).
//
// Home, Recents and Power map onto existing `OsEventType`s and Back and Volume onto the two new
// normalized input events, so a script reads like the stream the HAL would produce on a device.

#include "tessera/hal/synthetic_events.hpp"

namespace tessera::hal {
namespace {

// Home emits two OS events, so it needs two free slots or it is refused whole.
constexpr std::size_t kHomeEvents = 2;

ui::InputEvent volume_event(ui::VolumeDirection direction) {
    ui::InputEvent event;
    event.type = ui::InputEventType::VolumeKey;
    event.volume_direction = direction;
    return event;
}

} // namespace

InjectStatus SyntheticEventInjector::press(SystemButton button) {
    switch (button) {
    case SystemButton::Back: {
        ui::InputEvent event;
        event.type = ui::InputEventType::BackRequested;
        return push_input(event);
    }
    case SystemButton::VolumeUp:
        return push_input(volume_event(ui::VolumeDirection::Up));
    case SystemButton::VolumeDown:
        return push_input(volume_event(ui::VolumeDirection::Down));
    case SystemButton::Recents:
        return push(OsEventType::WindowDeactivated, EventReason::User);
    case SystemButton::Home: {
        if (kMaxSyntheticEvents - count_ < kHomeEvents) {
            ++overflows_;
            return InjectStatus::Full;
        }
        // Hidden first, then Suspend: the order a backgrounded Android activity reports them, and
        // lifecycle events are never merged (os-events-v1.md section 3).
        static_cast<void>(push(OsEventType::WindowHidden, EventReason::User));
        return push(OsEventType::Suspend, EventReason::User);
    }
    case SystemButton::Power: {
        const OsEventType type = screen_on_ ? OsEventType::ScreenOff : OsEventType::ScreenOn;
        const InjectStatus status = push(type, EventReason::User);
        if (status == InjectStatus::Queued) {
            screen_on_ = !screen_on_;
        }
        return status;
    }
    }
    return InjectStatus::UnknownType;
}

InjectStatus SyntheticEventInjector::push_input(ui::InputEvent event) {
    if (input_count_ == inputs_.size()) {
        ++overflows_;
        return InjectStatus::Full;
    }
    event.timestamp_s = static_cast<double>(now_ns_) / 1.0e9;
    inputs_[(input_head_ + input_count_) % inputs_.size()] = event;
    ++input_count_;
    return InjectStatus::Queued;
}

bool SyntheticEventInjector::pop_input(ui::InputEvent& out) noexcept {
    if (input_count_ == 0) {
        return false;
    }
    out = inputs_[input_head_];
    input_head_ = (input_head_ + 1) % inputs_.size();
    --input_count_;
    return true;
}

std::size_t SyntheticEventInjector::feed_input(ui::InputQueue& queue) {
    std::size_t fed = 0;
    while (input_count_ > 0 && queue.push(inputs_[input_head_])) {
        input_head_ = (input_head_ + 1) % inputs_.size();
        --input_count_;
        ++fed;
    }
    return fed;
}

SyntheticEventInjector make_back_pop_back_exit_script() {
    SyntheticEventInjector injector;
    for (int step = 0; step < 3; ++step) {
        static_cast<void>(injector.press(SystemButton::Back));
        injector.advance_ms(200);
    }
    return injector;
}

SyntheticEventInjector make_home_resume_script() {
    SyntheticEventInjector injector;
    static_cast<void>(injector.press(SystemButton::Home));
    injector.advance_ms(5000);
    static_cast<void>(injector.push(OsEventType::Resume, EventReason::User));
    static_cast<void>(injector.push(OsEventType::WindowShown, EventReason::User));
    return injector;
}

SyntheticEventInjector make_power_cycle_script() {
    SyntheticEventInjector injector;
    static_cast<void>(injector.press(SystemButton::Power));
    injector.advance_ms(1000);
    static_cast<void>(injector.press(SystemButton::Power));
    return injector;
}

SyntheticEventInjector make_process_death_script() {
    SyntheticEventInjector injector;
    static_cast<void>(injector.press(SystemButton::Home));
    return injector;
}

} // namespace tessera::hal
