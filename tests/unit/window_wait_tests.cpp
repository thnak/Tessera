// SPDX-License-Identifier: MIT

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "tessera/hal/timer/steady_time.hpp"
#include "tessera/hal/wake_source.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/hal/window_wait.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <windows.h>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "window_wait: " << message << "\n";
    }
    return condition;
}

HWND find_window(std::string_view title) {
    return FindWindowA(nullptr, std::string(title).c_str());
}

bool contains_close(const std::array<tessera::hal::WindowEvent, 16>& events, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (events[i].type == tessera::hal::WindowEventType::Close) {
            return true;
        }
    }
    return false;
}

// Posts WM_CLOSE from another thread after a short delay so the main thread is already idle in the
// wait when the message arrives.
std::thread post_close_soon(HWND window) {
    return std::thread([window]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        (void)PostMessageW(window, WM_CLOSE, 0, 0);
    });
}

} // namespace

int main() {
    using tessera::hal::WaitOutcome;
    using tessera::hal::WindowEvent;

    const std::string suffix = std::to_string(GetCurrentProcessId());
    tessera::hal::WindowDesc desc;
    desc.width = 320;
    desc.height = 200;
    desc.visible = false;

    // 1. An idle window times out instead of spinning.
    desc.title = "Tessera wait timeout " + suffix;
    auto window = tessera::hal::create_window(desc);
    if (!expect(window != nullptr, "create_window returns a window")) {
        return 1;
    }
    const HWND hwnd = find_window(desc.title);
    if (!expect(hwnd != nullptr, "native window exists")) {
        return 2;
    }

    double start = tessera::hal::steady_now_seconds();
    bool has_event = window->wait_for_events(0.03);
    double elapsed = tessera::hal::steady_now_seconds() - start;
    if (!expect(!has_event, "idle wait reports no event") ||
        !expect(elapsed >= 0.02, "idle wait blocks for the timeout") ||
        !expect(elapsed < 2.0, "idle wait is bounded")) {
        return 3;
    }

    // 2. A deadline in the past returns immediately without waiting.
    start = tessera::hal::steady_now_seconds();
    const WaitOutcome past = tessera::hal::wait_for_events_until(*window, start - 5.0);
    elapsed = tessera::hal::steady_now_seconds() - start;
    if (!expect(past == WaitOutcome::Deadline, "past deadline returns Deadline") ||
        !expect(elapsed < 0.5, "past deadline returns immediately")) {
        return 4;
    }

    // 2b. A frame-sized wait must land near its deadline rather than overshooting it. With the
    // default 15.6 ms system tick a 16.7 ms wait that misses the high-resolution timer sleeps until
    // the next tick boundary, about 31 ms, which halves a 60 fps loop. This is a bound, not proof
    // the timer is used: another process holding timeBeginPeriod(1) hides the problem.
    //
    // The bound reads the fastest of several waits, not one sample and not the median. Scheduler
    // noise only ever *adds* delay, so under load the distribution has a long right tail and any
    // central statistic drifts with it -- a cold-cache run with twelve parallel test processes has
    // pushed this median to 65 ms against the 40 ms bound while the timer was working correctly.
    // The minimum is the load-immune estimate: fifteen waits only needs one that was not
    // descheduled, measured at 16.68 ms idle and 16.68 ms with sixteen concurrent copies running.
    // A real regression inflates every sample, so the minimum moves with it and the bound still
    // trips.
    constexpr double kFrameWait = 1.0 / 60.0;
    constexpr int kFrameWaitSamples = 15;
    std::array<double, kFrameWaitSamples> frame_waits{};
    for (double& sample : frame_waits) {
        start = tessera::hal::steady_now_seconds();
        (void)window->wait_for_events(kFrameWait);
        sample = tessera::hal::steady_now_seconds() - start;
    }
    std::sort(frame_waits.begin(), frame_waits.end());
    // Sorting puts the fastest wait first. A wait cannot return before its deadline, so the fast
    // end is still a valid lower bound; the median and the worst sample only aid a future failure.
    const double fastest_wait = frame_waits.front();
    if (!expect(fastest_wait >= kFrameWait * 0.9, "frame-sized wait does not return early") ||
        !expect(fastest_wait < 0.040, "frame-sized wait lands near its deadline")) {
        std::cerr << "window_wait: fastest frame-sized wait " << fastest_wait * 1000.0
                  << " ms, median " << frame_waits[kFrameWaitSamples / 2] * 1000.0 << " ms, worst "
                  << frame_waits.back() * 1000.0 << " ms\n";
        return 12;
    }

    // 3. A posted WM_CLOSE wakes a blocked wait and is delivered as a normalized Close event.
    std::thread poster = post_close_soon(hwnd);
    start = tessera::hal::steady_now_seconds();
    has_event = window->wait_for_events(2.0);
    elapsed = tessera::hal::steady_now_seconds() - start;
    poster.join();
    if (!expect(has_event, "posted WM_CLOSE wakes the wait") ||
        !expect(elapsed < 2.0, "posted WM_CLOSE wakes quickly")) {
        return 5;
    }

    std::array<WindowEvent, 16> events{};
    std::size_t count = window->poll_events(events);
    if (!expect(contains_close(events, count), "poll_events yields Close after the wake")) {
        return 6;
    }
    // The delivered Close is no longer "pending", so a zero-timeout wait reports no event.
    if (!expect(!window->wait_for_events(-1.0), "delivered Close is not pending again")) {
        return 7;
    }
    window.reset();

    // 4. wait_for_events_until returns Event when a close wakes it before the future deadline.
    desc.title = "Tessera wait deadline " + suffix;
    auto deadline_window = tessera::hal::create_window(desc);
    if (!expect(deadline_window != nullptr, "create_window returns a second window")) {
        return 8;
    }
    const HWND deadline_hwnd = find_window(desc.title);
    if (!expect(deadline_hwnd != nullptr, "second native window exists")) {
        return 9;
    }

    std::thread deadline_poster = post_close_soon(deadline_hwnd);
    start = tessera::hal::steady_now_seconds();
    const WaitOutcome future = tessera::hal::wait_for_events_until(*deadline_window, start + 2.0);
    elapsed = tessera::hal::steady_now_seconds() - start;
    deadline_poster.join();
    if (!expect(future == WaitOutcome::Event, "future deadline returns Event on close") ||
        !expect(elapsed < 2.0, "future deadline close wake is quick")) {
        return 10;
    }

    count = deadline_window->poll_events(events);
    if (!expect(contains_close(events, count), "second window yields Close")) {
        return 11;
    }
    deadline_window.reset();

    // 6. The completion wake (issue #842). A worker's wake ends an idle park in milliseconds, not
    // at the timeout; a burst is one wake; and with no wake an idle window still times out, so a
    // static app does not spin.
    desc.title = "Tessera wait wake " + suffix;
    auto wake_window = tessera::hal::create_window(desc);
    const auto wake = tessera::hal::create_wake_source();
    if (!expect(wake_window != nullptr && wake != nullptr, "the wake window and source exist")) {
        return 12;
    }
    wake_window->set_wake_source(wake.get());
    start = tessera::hal::steady_now_seconds();
    has_event = wake_window->wait_for_events(0.05);
    elapsed = tessera::hal::steady_now_seconds() - start;
    if (!expect(!has_event, "an installed wake does not end an idle wait by itself") ||
        !expect(elapsed >= 0.03, "an idle wait with a wake still blocks for the timeout")) {
        return 13;
    }
    std::thread waker([&wake]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        wake->wake();
        wake->wake();
        wake->wake();
    });
    start = tessera::hal::steady_now_seconds();
    has_event = wake_window->wait_for_events(5.0);
    elapsed = tessera::hal::steady_now_seconds() - start;
    waker.join();
    if (!expect(has_event, "a worker wake ends the park with a wake-up") ||
        !expect(elapsed < 1.0, "the wake lands long before the 5 s timeout")) {
        return 14;
    }
    // The park above can return on the first wake of the burst; on a loaded runner the second
    // and third then land after it returned and are still pending. Consume those (at most the
    // two that were left), so the check below is about wakes not being sticky, not about how the
    // scheduler interleaved the waker thread.
    for (int drained = 0; drained < 2; ++drained) {
        if (!wake_window->wait_for_events(0.05)) {
            break;
        }
    }
    start = tessera::hal::steady_now_seconds();
    has_event = wake_window->wait_for_events(0.05);
    elapsed = tessera::hal::steady_now_seconds() - start;
    if (!expect(!has_event && elapsed >= 0.03,
                "a consumed burst of wakes leaves nothing pending: the next park blocks")) {
        return 15;
    }
    // Measured wake-to-return latency (issue #842): the point is "milliseconds, not the 250 ms
    // clamp", so the bound is generous and a loaded runner cannot make it flaky.
    constexpr int kTrials = 20;
    std::array<double, kTrials> latency{};
    for (int trial = 0; trial < kTrials; ++trial) {
        double woke_at = 0.0;
        std::thread trial_waker([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            woke_at = tessera::hal::steady_now_seconds();
            wake->wake();
            wake->wake();
            wake->wake();
        });
        has_event = wake_window->wait_for_events(5.0);
        const double returned_at = tessera::hal::steady_now_seconds();
        trial_waker.join();
        if (!expect(has_event, "a worker wake ends the park in every trial")) {
            return 16;
        }
        latency[static_cast<std::size_t>(trial)] = returned_at - woke_at;
        // Consume what is left of the burst so the next trial starts from a blocking park.
        while (wake_window->wait_for_events(0.02)) {
        }
    }
    std::sort(latency.begin(), latency.end());
    std::cout << "window_wait: wake-to-return latency median " << latency[kTrials / 2] * 1000.0
              << " ms, max " << latency.back() * 1000.0 << " ms over " << kTrials << " trials\n";
    if (!expect(latency.back() < 0.1, "every wake lands within 100 ms")) {
        return 17;
    }
    wake_window->set_wake_source(nullptr);
    wake_window.reset();

    std::cout << "window_wait: ok\n";
    return 0;
}
