// SPDX-License-Identifier: MIT
//
// Fixtures for the UIA host wiring tests: a logging apartment and a fake UIA platform seam. They
// share one ordered log so a test can assert that the root provider is created on the bridge thread
// after the apartment exists and released before the apartment shuts down. No test creates a COM
// object or talks to a real client.

#pragma once

#include "tessera/ui/accessibility/uia_host.hpp"

#include <condition_variable>
#include <cstddef>
#include <expected>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace a11y_uia_host_test {

// Ordered, thread-safe event log shared by the fixture halves.
class Log {
public:
    void add(std::string entry) {
        std::lock_guard lock(mutex_);
        entries_.push_back(std::move(entry));
    }

    [[nodiscard]] std::vector<std::string> snapshot() const {
        std::lock_guard lock(mutex_);
        return entries_;
    }

    [[nodiscard]] std::size_t count(std::string_view entry) const {
        std::lock_guard lock(mutex_);
        std::size_t matches = 0;
        for (const std::string& candidate : entries_) {
            if (candidate == entry) {
                ++matches;
            }
        }
        return matches;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> entries_;
};

class LoggingApartment final : public tessera::ui::a11y::UiaApartment {
public:
    explicit LoggingApartment(Log& log)
        : log_(log) {}

    [[nodiscard]] std::expected<void, tessera::ui::a11y::UiaError> init() override {
        log_.add("apartment.init");
        init_thread = std::this_thread::get_id();
        return result_;
    }

    void shutdown() override {
        log_.add("apartment.shutdown");
    }

    void pump() override {
        std::unique_lock lock(mutex_);
        ++pumps_;
        cv_.wait(lock, [this] { return !block_pump_; });
    }

    [[nodiscard]] std::string_view name() const override {
        return "logging";
    }

    void fail_init_with(tessera::ui::a11y::UiaError error) {
        result_ = std::unexpected(error);
    }

    void block_pump() {
        std::lock_guard lock(mutex_);
        block_pump_ = true;
    }

    void open_pump() {
        std::lock_guard lock(mutex_);
        block_pump_ = false;
        cv_.notify_all();
    }

    std::thread::id init_thread;

private:
    Log& log_;
    std::expected<void, tessera::ui::a11y::UiaError> result_{};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t pumps_ = 0;
    bool block_pump_ = false;
};

class FakePlatform final : public tessera::ui::a11y::UiaPlatform {
public:
    FakePlatform(Log& log, void* root)
        : log_(log),
          root_(root) {}

    void* create_root(tessera::ui::a11y::UiaBridge& bridge) override {
        log_.add("platform.create");
        create_thread = std::this_thread::get_id();
        running_at_create = bridge.running();
        return root_;
    }

    void destroy_root() override {
        log_.add("platform.destroy");
        destroy_thread = std::this_thread::get_id();
        ++destroys;
    }

    [[nodiscard]] std::size_t destroys_count() const {
        return destroys;
    }

    std::thread::id create_thread;
    std::thread::id destroy_thread;
    bool running_at_create = true;

private:
    Log& log_;
    void* root_;
    std::size_t destroys = 0;
};

// Index of `entry` in the log, or -1 when absent. Tests use it for ordering assertions.
inline std::int64_t index_of(const std::vector<std::string>& entries, std::string_view entry) {
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (entries[index] == entry) {
            return static_cast<std::int64_t>(index);
        }
    }
    return -1;
}

} // namespace a11y_uia_host_test
