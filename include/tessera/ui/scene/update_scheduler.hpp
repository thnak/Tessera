// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace tessera::ui {

class UpdateScheduler;

// One per generated component. A setter marks its ticket; the scheduler runs the component's
// `update()` once at the next flush. The ticket must keep a stable address between `attach` and
// `detach` (generated components are built in place and never moved while mounted).
struct UpdateTicket {
    using UpdateFn = void (*)(void* context);

    UpdateFn update = nullptr;
    void* context = nullptr;

    // Marks the owner dirty. A no-op while detached, so headless tests that call `update()`
    // by hand keep working. UI-thread only (asserted in debug builds).
    void schedule();

    [[nodiscard]] bool attached() const {
        return scheduler_ != nullptr;
    }
    [[nodiscard]] bool queued() const {
        return queued_;
    }

private:
    friend class UpdateScheduler;
    UpdateScheduler* scheduler_ = nullptr;
    bool queued_ = false;
};

// Counters of one `flush()`; `UpdateScheduler::total()` accumulates them.
struct UpdateFlushStats {
    std::uint32_t updated = 0;
    std::uint32_t passes = 0;
    // True when the pass budget ran out: remaining dirty tickets stay queued for the next flush
    // and the loop is woken again (a setter cycle is reported instead of spinning).
    bool pass_limit_hit = false;
};

// Per-window dirty-component queue (issue #5). Setters raise tickets; the host calls `flush()`
// once per tick after completions and input were drained and before style/layout, so one frame
// sees every state change made before it. Components flush in attach order, which is mount order:
// a parent mounts before its children, so a parent's `update()` runs first.
//
// Everything runs on the UI thread. Other threads must hand work over through the async
// completion queue and call setters from the completion callback; `schedule()` asserts the
// owner thread in debug builds and does no locking. No allocation after `reserve`.
class UpdateScheduler {
public:
    // Re-entrant dirtying (a setter called from `update()`) re-runs the pass; this bounds it.
    static constexpr std::uint32_t kDefaultMaxPasses = 8;

    // Wake hook, called on the dirty edge (clean -> dirty) so an idle render-on-demand loop
    // runs a frame. Typically forwards to `RuntimeLoop::request_frame()`.
    using WakeFn = void (*)(void* context);

    explicit UpdateScheduler(std::uint32_t max_passes = kDefaultMaxPasses);

    UpdateScheduler(const UpdateScheduler&) = delete;
    UpdateScheduler& operator=(const UpdateScheduler&) = delete;

    void set_wake(WakeFn wake, void* context);
    void reserve(std::size_t components);

    // Registers `ticket` (idempotent). `detach` removes it and clears its queued mark; both are
    // UI-thread operations. Detach every ticket before the scheduler or the component dies.
    void attach(UpdateTicket& ticket);
    void detach(UpdateTicket& ticket);

    // Runs each queued ticket's `update()` in attach order, repeating while updates re-dirty
    // tickets, up to the pass budget.
    UpdateFlushStats flush();

    [[nodiscard]] bool pending() const {
        return pending_;
    }
    [[nodiscard]] std::size_t attached_count() const {
        return tickets_.size();
    }
    [[nodiscard]] const UpdateFlushStats& total() const {
        return total_;
    }
    [[nodiscard]] std::uint64_t pass_limit_hits() const {
        return pass_limit_hits_;
    }

private:
    friend struct UpdateTicket;
    void mark(UpdateTicket& ticket);
    [[nodiscard]] bool on_owner_thread() const;

    std::vector<UpdateTicket*> tickets_;
    std::uint32_t max_passes_ = kDefaultMaxPasses;
    bool pending_ = false;
    bool flushing_ = false;
    WakeFn wake_ = nullptr;
    void* wake_context_ = nullptr;
    UpdateFlushStats total_;
    std::uint64_t pass_limit_hits_ = 0;
    std::thread::id owner_ = std::this_thread::get_id();
};

} // namespace tessera::ui
