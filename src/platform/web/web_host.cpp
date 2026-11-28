// SPDX-License-Identifier: MIT

#include "web_host.hpp"

namespace tessera::hal::web {

WebHost::WebHost(WebHostCallbacks callbacks, std::size_t queue_capacity)
    : callbacks_(callbacks),
      queue_(queue_capacity) {}

void WebHost::set_frame_fn(WebFrameFn fn, void* user) {
    frame_fn_ = fn;
    frame_user_ = user;
}

void WebHost::request_frame() {
    if (callbacks_.request_frame != nullptr) {
        callbacks_.request_frame(callbacks_.user);
    }
}

void WebHost::apply(bool request) {
    if (request) {
        request_frame();
    }
}

void WebHost::push(const ui::InputEvent& event) {
    if (!queue_.push(event)) {
        ++dropped_;
    }
    apply(scheduler_.wake());
}

void WebHost::on_pointer(const DomPointerSample& sample) {
    push(make_pointer_event(sample));
}

void WebHost::on_wheel(const DomWheelSample& sample) {
    push(make_wheel_event(sample));
}

void WebHost::on_key(const DomKeySample& sample) {
    push(make_key_event(sample));
    ui::InputEvent text;
    if (make_text_event(sample, text)) {
        push(text);
    }
}

void WebHost::on_focus(bool focused, double timestamp_ms) {
    ui::InputEvent event;
    event.type = focused ? ui::InputEventType::Focus : ui::InputEventType::Blur;
    event.timestamp_s = timestamp_ms / 1000.0;
    push(event);
}

void WebHost::on_resize(const WebSurfaceMetrics& metrics) {
    metrics_ = metrics;
    resized_ = true;
    apply(scheduler_.wake());
}

void WebHost::on_visibility(bool visible) {
    apply(scheduler_.set_visible(visible));
}

void WebHost::on_wake() {
    apply(scheduler_.wake());
}

bool WebHost::take_resize() {
    const bool value = resized_;
    resized_ = false;
    return value;
}

void WebHost::on_frame_callback(double timestamp_ms) {
    scheduler_.begin_callback();
    if (frame_fn_ == nullptr) {
        return;
    }
    const WebFrameResult result = frame_fn_(frame_user_, timestamp_ms / 1000.0);
    const FrameDecision decision = scheduler_.end_callback(result.tick, result.timer_wait_seconds);
    if (decision.request_frame) {
        request_frame();
    } else if (decision.wake_after_seconds < runtime::kNoDeadline &&
               callbacks_.wake_after != nullptr) {
        callbacks_.wake_after(callbacks_.user, decision.wake_after_seconds);
    }
}

} // namespace tessera::hal::web
