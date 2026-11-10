// SPDX-License-Identifier: MIT
//
// Deterministic fake of the COM apartment seam. It records which thread initialized and shut the
// apartment down, counts message pumps, can fail initialization, and can hold `pump` on a gate so a
// test can observe a bounded client wait while the bridge thread is busy.

#pragma once

#include "tessera/ui/accessibility/uia_apartment.hpp"

#include <condition_variable>
#include <cstddef>
#include <expected>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace a11y_uia_test {

class FakeApartment final : public tessera::ui::a11y::UiaApartment {
public:
    enum class EventKind { Init, Shutdown };
    struct Event {
        EventKind kind;
        std::thread::id thread;
    };

    [[nodiscard]] std::expected<void, tessera::ui::a11y::UiaError> init() override {
        std::lock_guard lock(mutex_);
        events_.push_back({EventKind::Init, std::this_thread::get_id()});
        ++inits_;
        return init_result_;
    }

    void shutdown() override {
        std::lock_guard lock(mutex_);
        events_.push_back({EventKind::Shutdown, std::this_thread::get_id()});
        ++shutdowns_;
    }

    void pump() override {
        std::unique_lock lock(mutex_);
        ++pumps_;
        cv_.wait(lock, [this] { return !block_pump_; });
    }

    [[nodiscard]] std::string_view name() const override {
        return "fake";
    }

    void fail_init_with(tessera::ui::a11y::UiaError error) {
        std::lock_guard lock(mutex_);
        init_result_ = std::unexpected(error);
    }

    void clear_init_failure() {
        std::lock_guard lock(mutex_);
        init_result_ = {};
    }

    void close_pump_gate() {
        std::lock_guard lock(mutex_);
        block_pump_ = true;
    }

    void open_pump_gate() {
        std::lock_guard lock(mutex_);
        block_pump_ = false;
        cv_.notify_all();
    }

    [[nodiscard]] std::vector<Event> events() const {
        std::lock_guard lock(mutex_);
        return events_;
    }

    [[nodiscard]] std::size_t inits() const {
        std::lock_guard lock(mutex_);
        return inits_;
    }

    [[nodiscard]] std::size_t shutdowns() const {
        std::lock_guard lock(mutex_);
        return shutdowns_;
    }

    [[nodiscard]] std::size_t pumps() const {
        std::lock_guard lock(mutex_);
        return pumps_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::expected<void, tessera::ui::a11y::UiaError> init_result_{};
    std::vector<Event> events_;
    std::size_t inits_ = 0;
    std::size_t shutdowns_ = 0;
    std::size_t pumps_ = 0;
    bool block_pump_ = false;
};

} // namespace a11y_uia_test
