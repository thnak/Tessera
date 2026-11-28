// SPDX-License-Identifier: MIT

// Issue #290: DOM event to queued InputEvent to requested frame, through WebHost with fake page
// imports. The acceptance "a static page idles with zero frame callbacks" is counted here.

#include "web_host.hpp"

#include <iostream>

namespace {

using namespace tessera;
using namespace tessera::hal::web;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "web_host: " << message << "\n";
        ++g_failures;
    }
}

struct FakePage {
    int frame_requests = 0;
    int timer_requests = 0;
    double last_timer_seconds = -1.0;
    int frames_run = 0;
    bool keep_busy = false;
    double timer_wait = runtime::kNoDeadline;
};

WebHostCallbacks callbacks_for(FakePage& page) {
    WebHostCallbacks callbacks;
    callbacks.user = &page;
    callbacks.request_frame = [](void* user) { ++static_cast<FakePage*>(user)->frame_requests; };
    callbacks.wake_after = [](void* user, double seconds) {
        auto* page = static_cast<FakePage*>(user);
        ++page->timer_requests;
        page->last_timer_seconds = seconds;
    };
    return callbacks;
}

WebFrameResult run_frame(void* user, double /*now_seconds*/) {
    auto* page = static_cast<FakePage*>(user);
    ++page->frames_run;
    WebFrameResult result;
    result.tick.idle = !page->keep_busy;
    result.timer_wait_seconds = page->timer_wait;
    return result;
}

} // namespace

int main() {
    FakePage page;
    WebHost host(callbacks_for(page));
    host.set_frame_fn(&run_frame, &page);

    // A click is one down and one up; both queue, but only one frame is requested.
    DomPointerSample down;
    down.phase = DomPointerPhase::Down;
    down.type = DomPointerType::Mouse;
    down.button = 0;
    down.x = 4.0F;
    down.y = 5.0F;
    host.on_pointer(down);
    DomPointerSample up = down;
    up.phase = DomPointerPhase::Up;
    host.on_pointer(up);
    expect(page.frame_requests == 1, "two pointer events share one frame request");
    expect(host.input_queue().size() == 2, "both events are queued");
    ui::InputEvent event;
    expect(host.input_queue().pop(event) && event.type == ui::InputEventType::PointerDown &&
               event.x == 4.0F,
           "queue order and payload");

    host.on_frame_callback(16.0);
    expect(page.frames_run == 1, "the frame step ran");
    expect(page.frame_requests == 1 && page.timer_requests == 0,
           "settled: no further frame callback and no timer");

    // Idle for many page-lifetime events that carry no work: resize-less visibility no-ops.
    host.on_visibility(true);
    expect(page.frame_requests == 1, "redundant visibility change requests nothing");

    // Keyboard: Enter is a key only; a letter is a key plus text, still one request.
    host.on_frame_callback(32.0);
    DomKeySample enter;
    enter.key = "Enter";
    host.on_key(enter);
    expect(page.frame_requests == 2, "key input wakes the page");
    host.on_frame_callback(48.0);
    DomKeySample letter;
    letter.key = "x";
    const std::size_t before = host.input_queue().size();
    host.on_key(letter);
    expect(host.input_queue().size() == before + 2, "letter key + letter text queued");
    expect(page.frame_requests == 3, "key and text share one request");

    // A busy loop (animation) keeps asking; a pending timer parks with one timeout.
    page.keep_busy = true;
    host.on_frame_callback(64.0);
    expect(page.frame_requests == 4, "pending work requests the next frame");
    page.keep_busy = false;
    page.timer_wait = 0.25;
    host.on_frame_callback(80.0);
    expect(page.frame_requests == 4 && page.timer_requests == 1 && page.last_timer_seconds == 0.25,
           "a future timer arms one timeout and no frame");
    page.timer_wait = runtime::kNoDeadline;

    // Resize is reported once and wakes the page.
    host.on_wake();
    host.on_frame_callback(96.0);
    const int requests = page.frame_requests;
    host.on_resize({800.0F, 600.0F, 2.0F});
    expect(page.frame_requests == requests + 1, "resize wakes the page");
    expect(host.take_resize() && !host.take_resize(), "resize is consumed once");
    expect(host.metrics().width == 800.0F && host.metrics().device_pixel_ratio == 2.0F, "metrics");

    // Hidden: input still queues but requests nothing; showing repaints once.
    host.on_frame_callback(112.0);
    host.on_visibility(false);
    const int hidden_requests = page.frame_requests;
    host.on_pointer(down);
    expect(page.frame_requests == hidden_requests, "no request while hidden");
    host.on_visibility(true);
    expect(page.frame_requests == hidden_requests + 1, "showing the page requests one frame");

    // Overflow is counted, never silent.
    WebHost small(callbacks_for(page), 2);
    for (int i = 0; i < 10; ++i) {
        small.on_pointer(down);
    }
    expect(small.dropped_events() > 0, "overflow is counted");

    return g_failures == 0 ? 0 : 1;
}
