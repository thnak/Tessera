// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>

namespace tessera::runtime {

class Clock;
class CompletionQueue;
class TimerQueue;

enum class CompletionPriority : std::uint8_t {
    Visible = 0,
    LayoutPaint = 1,
    Background = 2,
};

// Why a cancellation happened (issue #845). The first reason wins, the way the first `cancel()`
// does, so an awaiter can tell a user abort from a timeout from a shutdown without a side channel.
enum class CancelReason : std::uint8_t {
    // `cancel()` or a scope's explicit cancel: the work is no longer wanted.
    Requested = 0,
    // A `cancel_after` deadline or a timeout helper expired.
    Timeout,
    // The owner is shutting down (`TaskScope::close()` at app or service stop).
    Shutdown,
    // A linked source was cancelled because one of its parents was.
    Parent,
};

namespace detail {
struct CancellationState;
class ContinuationNode;
} // namespace detail

class CancellationSource;
class CancellationRegistration;

class CancellationToken {
public:
    CancellationToken() = default;

    [[nodiscard]] bool cancelled() const;
    // Why the token was cancelled, or `std::nullopt` while it is live.
    [[nodiscard]] std::optional<CancelReason> reason() const;

    // Registers `callback` to run once when this token is cancelled. The returned registration owns
    // the registration: destroying it (or calling `reset()`) unregisters the callback, so a
    // long-lived token never accumulates callbacks that exist only for one request.
    //
    // Thread rule (.NET's): callbacks run synchronously on the thread that calls `cancel()`. A
    // callback that must touch UI-owned state has to hand the work to the completion queue itself;
    // it must never mutate the scene graph from a worker thread.
    //
    // An already-cancelled token runs the callback immediately and returns an empty registration.
    // A callback that destroys its own registration does not deadlock; a registration destroyed on
    // another thread while the callback runs waits for it to finish, so a captured `this` stays
    // valid.
    [[nodiscard]] CancellationRegistration on_cancel(std::function<void()> callback) const;

    // Number of live registrations. For diagnostics and tests; it is the observable proof that
    // unregistering returned the list to its starting size.
    [[nodiscard]] std::size_t registration_count() const;

private:
    explicit CancellationToken(std::shared_ptr<detail::CancellationState> state)
        : state_(std::move(state)) {}

    std::shared_ptr<detail::CancellationState> state_;
    friend class CancellationSource;
};

// Move-only handle on one callback registration (issue #845). Empty for a token that was already
// cancelled, so the whole thing is `if (auto registration = token.on_cancel(f); ...)` free.
class CancellationRegistration {
public:
    CancellationRegistration() noexcept = default;
    CancellationRegistration(CancellationRegistration&& other) noexcept;
    CancellationRegistration& operator=(CancellationRegistration&& other) noexcept;
    ~CancellationRegistration();

    CancellationRegistration(const CancellationRegistration&) = delete;
    CancellationRegistration& operator=(const CancellationRegistration&) = delete;

    // Unregisters now (waiting for a running callback, as the destructor does). Idempotent.
    void reset() noexcept;
    [[nodiscard]] bool active() const noexcept {
        return state_ != nullptr;
    }

private:
    friend class CancellationToken;
    friend class CancellationSource;
    friend struct detail::CancellationState;
    CancellationRegistration(std::shared_ptr<detail::CancellationState> state, std::uint32_t slot,
                             std::uint32_t generation) noexcept;

    std::shared_ptr<detail::CancellationState> state_;
    std::uint32_t slot_ = 0;
    std::uint32_t generation_ = 0;
};

class CancellationSource {
public:
    CancellationSource();

    // Cancels once; the first reason wins and later calls are no-ops.
    void cancel(CancelReason reason = CancelReason::Requested);
    [[nodiscard]] bool cancelled() const;
    [[nodiscard]] CancellationRegistration on_cancel(std::function<void()> callback) const;
    [[nodiscard]] CancellationToken token() const;

    // A source cancelled when any of `parents` is cancelled (issue #845, .NET's
    // CreateLinkedTokenSource). It holds registrations on its parents, so destroying the linked
    // source unregisters from them instead of leaking one callback per link. The child's reason is
    // `CancelReason::Parent`; the parents must outlive the linked source.
    [[nodiscard]] static CancellationSource linked(std::span<const CancellationToken> parents);
    // Variadic convenience: `linked(a.token(), b.token())` and `linked(a.when, b.when)` both work,
    // because the arguments are copied into a small array before the span is built.
    template<typename... Tokens>
    [[nodiscard]] static CancellationSource linked(const Tokens&... parents) {
        const CancellationToken tokens[] = {parents...};
        return linked(std::span<const CancellationToken>(tokens, sizeof...(parents)));
    }

    // Cancels this source `seconds` from now with `CancelReason::Timeout` (issue #845). The
    // deadline is absolute on `clock`, so a `ManualClock` fires it deterministically. Calling it
    // again moves the deadline. A no-op after `cancel()`. The timer is removed when the last handle
    // to this source is destroyed, so `timers` and `clock` must outlive it. False when the timer
    // queue is full (the source stays live).
    bool cancel_after(Clock& clock, TimerQueue& timers, double seconds);

    // Disarms an armed `cancel_after` deadline without cancelling the source (issue #844). The
    // timeout helper calls it when the child wins the race: the deadline is moot, and leaving it
    // armed would cancel a later operation on this source at a deadline nobody expects. A no-op
    // when no deadline is armed.
    void cancel_deadline();

    // True once an armed `cancel_after` deadline elapsed and its timer callback ran (issue #844).
    // This is how an awaiter racing a deadline tells "the deadline elapsed" from "someone called
    // `cancel()`". Sticky: a re-armed deadline sets it again on its own fire.
    [[nodiscard]] bool deadline_fired() const;

private:
    std::shared_ptr<detail::CancellationState> state_;
};

// One queued task resume, linked intrusively (issue #843). Task continuations are not
// capacity-bounded and are never dropped: the number of queued resumes is bounded by the number of
// live tasks, which `TaskScope` already caps. Threading the link through the task state instead of
// a `std::function` is also what makes a continuation post allocation-free.
namespace detail {
class ContinuationNode {
public:
    ContinuationNode() = default;
    virtual ~ContinuationNode() = default;

    ContinuationNode(const ContinuationNode&) = delete;
    ContinuationNode& operator=(const ContinuationNode&) = delete;

    // Runs the queued resume. Implementations drop their queued keep-alive reference here.
    virtual void resume_continuation() = 0;

    // Hands back the keep-alive reference a queued node holds, so a queue that dies with resumes
    // still pending can release them (a self-referencing queued task is otherwise a leak).
    virtual std::shared_ptr<void> take_queued_reference() noexcept {
        return {};
    }

private:
    friend class tessera::runtime::CompletionQueue;
    ContinuationNode* next_ = nullptr;
    bool linked_ = false;
};
} // namespace detail

class CompletionQueue {
public:
    struct Stats {
        std::size_t posted = 0;
        std::size_t executed = 0;
        std::size_t cancelled = 0;
        std::size_t dropped = 0;
        std::size_t discarded = 0;
        std::size_t pending = 0;
        std::size_t pending_visible = 0;
        std::size_t pending_layout = 0;
        std::size_t pending_background = 0;
        // Queued task resumes (issue #843). They are not capacity-bounded and are never dropped, so
        // `dropped` counts only fire-and-forget completions.
        std::size_t pending_continuations = 0;
        std::size_t continuations_executed = 0;
    };

    // Wakes the parked UI thread when a completion is posted (issue #842). Installed by the host;
    // called from any thread, at most once per empty -> non-empty edge, so a burst of posts wakes
    // once. A null wake (the default) keeps the old idle-until-timeout behaviour.
    using WakeFn = void (*)(void* context);

    explicit CompletionQueue(std::size_t capacity = 256, WakeFn wake = nullptr,
                             void* wake_context = nullptr);
    // Releases the keep-alive of every still-queued task resume. Those tasks never run (the queue
    // is gone), and their queued self-reference would otherwise leak the whole frame.
    ~CompletionQueue();
    CompletionQueue(const CompletionQueue&) = delete;
    CompletionQueue& operator=(const CompletionQueue&) = delete;

    void set_wake(WakeFn wake, void* wake_context) noexcept;

    bool post(CompletionPriority priority, std::function<void()> task,
              CancellationToken token = {});

    // Queues a task resume on the unbounded continuation lane at `priority` (issue #843). It is
    // never dropped, so a resume cannot vanish and hang its task; a burst is spread over ticks by
    // the per-tick budget. Continuations at a priority drain before plain completions at the same
    // priority, so a task is never starved behind fire-and-forget work. A node already queued is
    // refused, so a double resume cannot corrupt the list. Any thread.
    bool post_continuation(CompletionPriority priority, detail::ContinuationNode& node);

    std::size_t run(std::size_t budget);
    std::size_t clear();

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool empty() const;
    [[nodiscard]] std::size_t capacity() const {
        return capacity_;
    }
    [[nodiscard]] std::size_t pending(CompletionPriority priority) const;
    [[nodiscard]] Stats stats() const;

private:
    struct Task {
        std::function<void()> function;
        CancellationToken token;
    };

    static std::size_t priority_index(CompletionPriority priority);
    // The shared slow path of `post`: a wake is fired when the queue went from empty to non-empty.
    void wake_if_newly_non_empty() noexcept;
    [[nodiscard]] bool is_empty_locked() const noexcept;

    mutable std::mutex mutex_;
    std::deque<Task> visible_;
    std::deque<Task> layout_paint_;
    std::deque<Task> background_;
    // Intrusive FIFOs of task resumes, one per priority, allocated by the task states that own
    // them. Kept across `clear()` on purpose: a resume is not fire-and-forget work, and dropping
    // one is the hang #843 exists to remove.
    struct ContinuationLane {
        detail::ContinuationNode* head = nullptr;
        detail::ContinuationNode* tail = nullptr;
    };
    ContinuationLane continuations_[3];
    std::size_t capacity_ = 0;
    Stats stats_;
    WakeFn wake_ = nullptr;
    void* wake_context_ = nullptr;
    bool was_empty_ = true;
};

class ManualExecutor {
public:
    struct Stats {
        std::size_t posted = 0;
        std::size_t executed = 0;
        std::size_t cancelled = 0;
        std::size_t dropped = 0;
        std::size_t discarded = 0;
        std::size_t pending = 0;
    };

    explicit ManualExecutor(std::size_t capacity = 256);

    bool post(std::function<void()> task, CancellationToken token = {});

    bool run_one();
    std::size_t run(std::size_t budget);

    std::size_t clear();

    [[nodiscard]] std::size_t size() const {
        return tasks_.size();
    }
    [[nodiscard]] bool empty() const {
        return tasks_.empty();
    }
    [[nodiscard]] std::size_t capacity() const {
        return capacity_;
    }
    [[nodiscard]] Stats stats() const;

private:
    struct Task {
        std::function<void()> function;
        CancellationToken token;
    };

    std::deque<Task> tasks_;
    std::size_t capacity_ = 0;
    Stats stats_;
};

} // namespace tessera::runtime
