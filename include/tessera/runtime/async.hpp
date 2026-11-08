// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <utility>

namespace tessera::runtime {

namespace detail {
struct CancellationState;
}

class CancellationSource;

class CancellationToken {
public:
    CancellationToken() = default;

    [[nodiscard]] bool cancelled() const;

private:
    explicit CancellationToken(std::shared_ptr<const detail::CancellationState> state)
        : state_(std::move(state)) {}

    std::shared_ptr<const detail::CancellationState> state_;
    friend class CancellationSource;
};

class CancellationSource {
public:
    CancellationSource();

    void cancel();
    [[nodiscard]] bool cancelled() const;
    [[nodiscard]] CancellationToken token() const;

private:
    std::shared_ptr<detail::CancellationState> state_;
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
