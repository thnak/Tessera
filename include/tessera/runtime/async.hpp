// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <utility>

namespace tessera::runtime {

enum class CompletionPriority {
    Visible = 0,
    LayoutPaint = 1,
    Background = 2,
};

namespace detail {
struct CancellationState;
}

class CancellationSource;

class CancellationToken {
public:
    CancellationToken() = default;

    [[nodiscard]] bool cancelled() const;
    void on_cancel(std::function<void()> callback) const;

private:
    explicit CancellationToken(std::shared_ptr<detail::CancellationState> state)
        : state_(std::move(state)) {}

    std::shared_ptr<detail::CancellationState> state_;
    friend class CancellationSource;
};

class CancellationSource {
public:
    CancellationSource();

    void cancel();
    [[nodiscard]] bool cancelled() const;
    void on_cancel(std::function<void()> callback) const;
    [[nodiscard]] CancellationToken token() const;

private:
    std::shared_ptr<detail::CancellationState> state_;
};

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
    };

    explicit CompletionQueue(std::size_t capacity = 256);

    bool post(CompletionPriority priority, std::function<void()> task,
              CancellationToken token = {});
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

    std::deque<Task> visible_;
    std::deque<Task> layout_paint_;
    std::deque<Task> background_;
    std::size_t capacity_ = 0;
    Stats stats_;
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
