// SPDX-License-Identifier: MIT

// Issue #290: the rAF policy (ADR-046 item 5). A parked page holds no frame callback.

#include "web_frame_scheduler.hpp"

#include <iostream>

namespace {

using namespace tessera;
using namespace tessera::hal::web;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "web_frame_scheduler: " << message << "\n";
        ++g_failures;
    }
}

runtime::RuntimeTick idle_tick() {
    runtime::RuntimeTick tick;
    tick.idle = true;
    return tick;
}

} // namespace

int main() {
    {
        FrameCallbackScheduler scheduler;
        expect(!scheduler.callback_outstanding(), "a new page requests nothing by itself");
        expect(scheduler.wake(), "the first wake requests a frame");
        expect(!scheduler.wake() && !scheduler.wake(),
               "wakes coalesce into one outstanding callback");
        scheduler.begin_callback();
        const auto decision = scheduler.end_callback(idle_tick(), runtime::kNoDeadline);
        expect(!decision.request_frame, "an idle tick requests no frame");
        expect(decision.wake_after_seconds == runtime::kNoDeadline, "an idle tick arms no timer");
        expect(!scheduler.callback_outstanding(), "zero callbacks outstanding while parked");
        expect(scheduler.wake(), "input after parking requests a frame again");
    }
    {
        FrameCallbackScheduler scheduler;
        (void)scheduler.wake();
        scheduler.begin_callback();
        runtime::RuntimeTick busy;
        busy.idle = false;
        expect(scheduler.end_callback(busy, runtime::kNoDeadline).request_frame,
               "pending work continues");
        expect(scheduler.callback_outstanding(), "the continued frame is outstanding");
        scheduler.begin_callback();
        runtime::RuntimeTick completions = idle_tick();
        completions.completions_pending = true;
        expect(scheduler.end_callback(completions, runtime::kNoDeadline).request_frame,
               "pending completions continue");
        scheduler.begin_callback();
        expect(scheduler.end_callback(idle_tick(), 0.0).request_frame, "a due timer continues");
        scheduler.begin_callback();
        const auto parked = scheduler.end_callback(idle_tick(), 0.5);
        expect(!parked.request_frame && parked.wake_after_seconds == 0.5,
               "a future timer arms one timeout and no frame");
    }
    {
        FrameCallbackScheduler scheduler;
        (void)scheduler.wake();
        scheduler.begin_callback();
        runtime::RuntimeTick blocked;
        blocked.idle = false;
        blocked.device_blocked = true;
        const auto decision = scheduler.end_callback(blocked, 0.0);
        expect(!decision.request_frame && decision.wake_after_seconds == kDeviceBlockedRetrySeconds,
               "a lost device retries on a timer, never a rAF spin");
    }
    {
        FrameCallbackScheduler scheduler;
        expect(!scheduler.set_visible(false), "hiding requests nothing");
        expect(!scheduler.wake(), "a wake while hidden requests nothing");
        expect(scheduler.set_visible(true), "showing answers the remembered wake");
        expect(!scheduler.set_visible(true), "visible to visible is not a transition");
        scheduler.begin_callback();
        (void)scheduler.set_visible(false);
        runtime::RuntimeTick busy;
        busy.idle = false;
        expect(!scheduler.end_callback(busy, 0.0).request_frame, "a hidden page stops the loop");
        expect(scheduler.set_visible(true), "becoming visible repaints once");
    }
    return g_failures == 0 ? 0 : 1;
}
