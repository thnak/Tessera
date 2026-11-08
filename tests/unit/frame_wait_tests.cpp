// SPDX-License-Identifier: MIT

#include "tessera/runtime/frame_wait.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>

namespace {

using tessera::hal::Window;
using tessera::hal::WindowEvent;
using tessera::hal::WindowMetrics;
using tessera::runtime::FramePacer;
using tessera::runtime::FramePacerConfig;
using tessera::runtime::wait_for_next_frame;

class FakeWindow final : public Window {
public:
    explicit FakeWindow(bool wake)
        : wake_(wake) {}

    [[nodiscard]] WindowMetrics metrics() const override {
        return {};
    }
    std::size_t poll_events(std::span<WindowEvent>) override {
        return 0;
    }
    void show() override {}
    void hide() override {}
    void request_redraw() override {}
    void close() override {}
    bool wait_for_events(double timeout_seconds) override {
        ++wait_calls;
        last_timeout = timeout_seconds;
        return wake_;
    }

    int wait_calls = 0;
    double last_timeout = 0.0;

private:
    bool wake_ = false;
};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "frame_wait: " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    FramePacer ready_pacer(FramePacerConfig{0.0});
    ready_pacer.present(0.0);
    FakeWindow ready_window(false);
    const auto immediate = wait_for_next_frame(ready_window, ready_pacer);
    if (!expect(immediate.deadline && !immediate.event, "uncapped pacer is ready immediately") ||
        !expect(ready_window.wait_calls == 0, "no wait when ready")) {
        return 1;
    }

    FramePacer event_pacer(FramePacerConfig{30.0});
    event_pacer.present(tessera::hal::steady_now_seconds());
    FakeWindow event_window(true);
    const auto event = wait_for_next_frame(event_window, event_pacer);
    if (!expect(event.event && !event.deadline, "window event wins") ||
        !expect(event_window.wait_calls == 1, "one bounded wait") ||
        !expect(event_window.last_timeout > 0.0 && event_window.last_timeout <= 0.05,
                "wait slice is bounded")) {
        return 2;
    }

    FramePacer deadline_pacer(FramePacerConfig{200.0});
    deadline_pacer.present(tessera::hal::steady_now_seconds());
    FakeWindow deadline_window(false);
    const auto deadline = wait_for_next_frame(deadline_window, deadline_pacer);
    if (!expect(deadline.deadline && !deadline.event, "deadline without events") ||
        !expect(deadline_window.wait_calls >= 1, "waited for the deadline") ||
        !expect(deadline_window.last_timeout > 0.0, "positive bounded slice")) {
        return 3;
    }

    std::cout << "frame_wait: ok\n";
    return 0;
}
