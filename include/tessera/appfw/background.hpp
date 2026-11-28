// SPDX-License-Identifier: MIT
//
// `BackgroundCoordinator`: the in-process background-work policy (issue #453, ADR-050 section 5
// items 3 and 4; design note: docs/design/background-policy.md).
//
// It turns the normalized `AppState` into the three actions the policy names, and nothing else:
//
//   * entering `Hidden` or `Suspended` from a visible state runs each registered checkpoint once
//     ("ask every service to checkpoint"), so work is saved before the OS may freeze the process;
//   * entering `Suspended` pauses the timer queue and cancels every registered scope whose
//     `BackgroundPolicy` is not `Continue` (cooperative cancel only: the scope's own close/drain
//     rules still apply, and nothing here waits);
//   * leaving `Suspended` resumes the timer queue. A cancelled scope stays cancelled: the owner
//     builds a fresh scope on resume, because a cancel is a one-way flip by design.
//
// OS-scheduled jobs (WorkManager, BGTaskScheduler) are deliberately not here; the design note
// records why they are not V1.
//
// Registration storage is one allocation at construction, so `on_app_state` allocates nothing and a
// full coordinator refuses instead of growing. Everything runs on the UI thread.

#pragma once

#include "tessera/appfw/lifecycle.hpp"
#include "tessera/appfw/task_scope.hpp"
#include "tessera/runtime/timer_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace tessera::appfw {

// How a scope's work behaves once the app is no longer visible. `Continue` is the desktop default
// (services keep running while `Hidden`); `Throttle` and `Pause` are the mobile-friendly choices
// and are both cancelled at `Suspended`, because a frozen process cannot honor either.
enum class BackgroundPolicy : std::uint8_t {
    Continue,
    Throttle,
    Pause,
};

[[nodiscard]] constexpr const char* to_string(BackgroundPolicy policy) noexcept {
    switch (policy) {
    case BackgroundPolicy::Continue:
        return "continue";
    case BackgroundPolicy::Throttle:
        return "throttle";
    case BackgroundPolicy::Pause:
        return "pause";
    }
    return "continue";
}

struct BackgroundRegistration {
    // Borrowed; must outlive the registration (call `unregister` first). Null is refused.
    TaskScope* scope = nullptr;
    BackgroundPolicy policy = BackgroundPolicy::Continue;
    // Optional. Runs on the UI thread once per entry into the background; must not block.
    std::function<void()> checkpoint;
};

class BackgroundCoordinator {
public:
    static constexpr std::size_t kDefaultCapacity = 32;

    // `timers` is optional and borrowed: when set, `Suspended` pauses it.
    explicit BackgroundCoordinator(runtime::TimerQueue* timers = nullptr,
                                   std::size_t capacity = kDefaultCapacity);

    BackgroundCoordinator(const BackgroundCoordinator&) = delete;
    BackgroundCoordinator& operator=(const BackgroundCoordinator&) = delete;

    // Returns an id that is never reused, or nullopt when full or the scope is null.
    [[nodiscard]] std::optional<std::uint32_t> register_scope(BackgroundRegistration registration);
    bool unregister(std::uint32_t id);

    // Feeds the state the app just entered. Idempotent for a repeated state.
    void on_app_state(AppState state);

    [[nodiscard]] bool backgrounded() const noexcept {
        return backgrounded_;
    }
    [[nodiscard]] std::size_t registered() const noexcept {
        return count_;
    }
    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }
    [[nodiscard]] std::size_t refused() const noexcept {
        return refused_;
    }
    // Cumulative counters: the deterministic evidence a test (or a diagnostics panel) reads.
    [[nodiscard]] std::size_t checkpoints_run() const noexcept {
        return checkpoints_run_;
    }
    [[nodiscard]] std::size_t scopes_cancelled() const noexcept {
        return scopes_cancelled_;
    }

private:
    struct Slot {
        std::uint32_t id = 0;
        bool used = false;
        BackgroundRegistration registration;
    };

    void enter_background();
    void enter_suspended();
    void leave_suspended();

    runtime::TimerQueue* timers_ = nullptr;
    std::unique_ptr<Slot[]> slots_;
    std::size_t capacity_ = 1;
    std::size_t count_ = 0;
    std::uint32_t next_id_ = 1;
    std::size_t refused_ = 0;
    std::size_t checkpoints_run_ = 0;
    std::size_t scopes_cancelled_ = 0;
    bool backgrounded_ = false;
    bool suspended_ = false;
};

} // namespace tessera::appfw
