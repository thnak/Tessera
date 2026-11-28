// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/input.hpp"
#include "web_frame_scheduler.hpp"
#include "web_input_translation.hpp"

// The browser HAL as one object (ADR-046 item 3 and 5): the page's flattened DOM events come in,
// normalized `InputEvent`s land in the shared queue, and the scheduler turns "something changed"
// into at most one `requestAnimationFrame`. It has no Emscripten dependency, so the whole path
// from a DOM event to a requested frame runs in a host unit test; only `web_exports.cpp` (the
// extern "C" surface and the two JS imports) needs the Emscripten toolchain.

namespace tessera::hal::web {

// The two things the host asks the page to do. Plain function pointers: the page imports are
// free functions and the host never owns a callback object.
struct WebHostCallbacks {
    void (*request_frame)(void* user) = nullptr;
    void (*wake_after)(void* user, double seconds) = nullptr;
    void* user = nullptr;
};

// What the app's frame step reports back after one `RuntimeLoop::tick` and its work.
struct WebFrameResult {
    runtime::RuntimeTick tick;
    double timer_wait_seconds = runtime::kNoDeadline;
};

using WebFrameFn = WebFrameResult (*)(void* user, double now_seconds);

// Canvas size in CSS pixels and the device pixel ratio the backing store is scaled by.
struct WebSurfaceMetrics {
    float width = 0.0F;
    float height = 0.0F;
    float device_pixel_ratio = 1.0F;
};

class WebHost {
public:
    explicit WebHost(WebHostCallbacks callbacks, std::size_t queue_capacity = 256);

    // The app registers its frame step once, before the first event.
    void set_frame_fn(WebFrameFn fn, void* user);

    void on_pointer(const DomPointerSample& sample);
    void on_wheel(const DomWheelSample& sample);
    // A key-down pushes a KeyDown and, when it types, a Text event; a key-up pushes a KeyUp.
    void on_key(const DomKeySample& sample);
    void on_focus(bool focused, double timestamp_ms);
    void on_resize(const WebSurfaceMetrics& metrics);
    void on_visibility(bool visible);
    // A completion, timer or other non-input source asks for a frame.
    void on_wake();

    // The `requestAnimationFrame` callback. Runs the app's frame step and arms the next wake.
    void on_frame_callback(double timestamp_ms);

    [[nodiscard]] ui::InputQueue& input_queue() {
        return queue_;
    }
    [[nodiscard]] const WebSurfaceMetrics& metrics() const {
        return metrics_;
    }
    // True once after a resize, so the app recreates its surface at the new size.
    [[nodiscard]] bool take_resize();
    [[nodiscard]] const FrameCallbackScheduler& scheduler() const {
        return scheduler_;
    }
    // Events dropped because the queue was full since construction; the page reports it.
    [[nodiscard]] std::size_t dropped_events() const {
        return dropped_;
    }

private:
    void push(const ui::InputEvent& event);
    void apply(bool request);
    void request_frame();

    WebHostCallbacks callbacks_;
    FrameCallbackScheduler scheduler_;
    ui::InputQueue queue_;
    WebFrameFn frame_fn_ = nullptr;
    void* frame_user_ = nullptr;
    WebSurfaceMetrics metrics_;
    bool resized_ = false;
    std::size_t dropped_ = 0;
};

} // namespace tessera::hal::web
