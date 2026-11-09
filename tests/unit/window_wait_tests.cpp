// SPDX-License-Identifier: MIT

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "tessera/hal/timer/steady_time.hpp"
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

    // 2b. A frame-sized wait must land near its deadline. With the default 15.6 ms system tick a
    // 16.7 ms wait without the high-resolution timer sleeps about 31 ms, which halves a 60 fps
    // loop. This only fails while the system tick is coarse: another process holding
    // timeBeginPeriod(1) hides the problem, so it is a bound, not proof the timer is used. The
    // median of several waits keeps one scheduler hiccup from failing the test.
    constexpr double kFrameWait = 1.0 / 60.0;
    constexpr int kFrameWaitSamples = 15;
    std::array<double, kFrameWaitSamples> frame_waits{};
    for (double& sample : frame_waits) {
        start = tessera::hal::steady_now_seconds();
        (void)window->wait_for_events(kFrameWait);
        sample = tessera::hal::steady_now_seconds() - start;
    }
    std::sort(frame_waits.begin(), frame_waits.end());
    const double median_wait = frame_waits[kFrameWaitSamples / 2];
    if (!expect(median_wait >= kFrameWait * 0.9, "frame-sized wait does not return early") ||
        !expect(median_wait < 0.024, "frame-sized wait lands near its deadline")) {
        std::cerr << "window_wait: median frame-sized wait " << median_wait * 1000.0 << " ms\n";
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

    std::cout << "window_wait: ok\n";
    return 0;
}
