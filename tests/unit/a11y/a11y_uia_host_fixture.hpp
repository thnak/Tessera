// SPDX-License-Identifier: MIT
//
// Fixtures for the UIA host wiring tests: a logging apartment and a fake UIA platform seam. They
// share one ordered log so a test can assert that the root provider is created on the bridge thread
// after the apartment exists and released before the apartment shuts down. No test creates a COM
// object or talks to a real client.

#pragma once

#include "tessera/ui/accessibility/uia_host.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
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

// The pump gate, in its own object so it can outlive the apartment that uses it. `HostHarness`
// needs to open the gate during teardown, but the apartment is owned by the bridge and is already
// destroyed by the time the harness's destructor runs (`AccessibilityHost::stop()` tears the host
// down first). Holding the gate separately means the harness can still release a parked worker
// without touching freed memory.
class PumpGate {
public:
    void block() {
        std::lock_guard lock(mutex_);
        blocked_ = true;
    }

    void open() {
        std::unique_lock lock(mutex_);
        blocked_ = false;
        cv_.notify_all();
        // Wait until the worker has actually left `pump()`. Without this, a caller that opens and
        // immediately re-blocks can flip `blocked_` back to true while the worker is still inside
        // the wait, so the worker never returns from that pump() call, never picks up the next job,
        // and the following `step()` times out.
        cv_.wait(lock, [this] { return !parked_; });
    }

    // The worker's side: parks while the gate is closed, and reports when it is inside the wait.
    void park() {
        std::unique_lock lock(mutex_);
        if (blocked_) {
            parked_ = true;
            cv_.notify_all();
        }
        cv_.wait(lock, [this] { return !blocked_; });
        parked_ = false;
        // Wake `open()`, which waits for this to clear before it returns.
        cv_.notify_all();
    }

    // The worker only honors the gate inside `pump()`; while it waits for a job it would run a
    // posted flush straight away. A test that needs "pending" to be deterministic must therefore
    // wait until the worker is really parked, after a barrier that forces one trip around its loop.
    [[nodiscard]] bool wait_parked() {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, std::chrono::seconds(5), [this] { return parked_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool blocked_ = false;
    bool parked_ = false;
};

class LoggingApartment final : public tessera::ui::a11y::UiaApartment {
public:
    // `gate` defaults to a private one, so a fixture that does not need to park the worker (and so
    // has nothing to release at teardown) can construct the apartment with just the log.
    explicit LoggingApartment(Log& log,
                              std::shared_ptr<PumpGate> gate = std::make_shared<PumpGate>())
        : log_(log),
          gate_(std::move(gate)) {}

    [[nodiscard]] std::expected<void, tessera::ui::a11y::UiaError> init() override {
        log_.add("apartment.init");
        init_thread = std::this_thread::get_id();
        return result_;
    }

    void shutdown() override {
        log_.add("apartment.shutdown");
    }

    void pump() override {
        gate_->park();
    }

    [[nodiscard]] std::string_view name() const override {
        return "logging";
    }

    void fail_init_with(tessera::ui::a11y::UiaError error) {
        result_ = std::unexpected(error);
    }

    void block_pump() {
        gate_->block();
    }

    [[nodiscard]] bool wait_parked() {
        return gate_->wait_parked();
    }

    void open_pump() {
        gate_->open();
    }

    std::thread::id init_thread;

private:
    Log& log_;
    std::shared_ptr<PumpGate> gate_;
    std::expected<void, tessera::ui::a11y::UiaError> result_{};
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

    // Window thread, before start: the host forwards the opaque handle and the fake records it. A
    // repeated identical install is a no-op, so the install path can be asserted idempotent.
    void set_host_handle(tessera::hal::AccessibilityHostHandle handle) override {
        log_.add("platform.set_handle");
        if (handle.token == handle_.token) {
            return;
        }
        handle_ = handle;
        ++handle_sets;
    }

    [[nodiscard]] tessera::ui::a11y::UiaHostElementStatus host_element_status() const override {
        tessera::ui::a11y::UiaHostElementStatus status;
        status.handle_installed = handle_.valid();
        status.state = handle_.valid() ? tessera::ui::a11y::UiaHostElementState::Pending
                                       : tessera::ui::a11y::UiaHostElementState::Unavailable;
        return status;
    }

    [[nodiscard]] tessera::ui::a11y::UiaError
    raise(std::span<const tessera::ui::a11y::UiaEvent> events) override {
        log_.add("platform.raise");
        std::lock_guard lock(mutex_);
        raise_thread = std::this_thread::get_id();
        batches_.push_back({std::this_thread::get_id(), {events.begin(), events.end()}});
        return tessera::ui::a11y::UiaError::None;
    }

    [[nodiscard]] std::size_t destroys_count() const {
        return destroys;
    }

    struct RaiseBatch {
        std::thread::id thread;
        std::vector<tessera::ui::a11y::UiaEvent> events;
    };

    [[nodiscard]] std::vector<RaiseBatch> batches() const {
        std::lock_guard lock(mutex_);
        return batches_;
    }

    std::thread::id create_thread;
    std::thread::id destroy_thread;
    bool running_at_create = true;
    tessera::hal::AccessibilityHostHandle handle_{};
    std::size_t handle_sets = 0;

private:
    Log& log_;
    void* root_;
    std::size_t destroys = 0;
    mutable std::mutex mutex_;
    std::vector<RaiseBatch> batches_;
    std::thread::id raise_thread;
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

// A long idle wait keeps the worker asleep unless a `step` barrier wakes it, so host assertions
// never depend on thread scheduling.
inline tessera::ui::a11y::UiaHostConfig test_config() {
    tessera::ui::a11y::UiaHostConfig config;
    config.bridge.idle_wait_ms = 1000;
    config.bridge.shutdown_wait_ms = 5000;
    return config;
}

// Owns the fakes and the host. Declaration order matters: the host is destroyed first, so the
// bridge is joined before the fake apartment/platform it points at are destroyed.
struct HostHarness {
    // The bridge's destructor joins its worker thread, and the worker can be parked inside
    // `LoggingApartment::pump()` waiting for the gate to open. Joining a parked worker deadlocks:
    // the gate is only opened by the test, which is already gone. Opening it here lets the worker
    // finish its pump, see the stop request and exit. This is the teardown half of the same
    // contract `open_pump()` documents.
    //
    // The gate is owned here, not reached through `apartment`: a host that tears itself down
    // (`AccessibilityHost::stop()`) destroys the apartment before this destructor runs, so the
    // `apartment` pointer is already dangling by then.
    ~HostHarness() {
        gate->open();
    }

    Log log;
    std::shared_ptr<PumpGate> gate = std::make_shared<PumpGate>();
    LoggingApartment* apartment = nullptr;
    FakePlatform* platform = nullptr;
    std::unique_ptr<tessera::ui::a11y::UiaHost> host;
};

inline std::unique_ptr<HostHarness>
make_host(tessera::ui::a11y::UiaHostConfig config = test_config(),
          void* root = reinterpret_cast<void*>(0x1234)) {
    auto harness = std::make_unique<HostHarness>();
    auto apartment = std::make_unique<LoggingApartment>(harness->log, harness->gate);
    harness->apartment = apartment.get();
    auto platform = std::make_unique<FakePlatform>(harness->log, root);
    harness->platform = platform.get();
    harness->host = std::make_unique<tessera::ui::a11y::UiaHost>(config, std::move(apartment),
                                                                 std::move(platform));
    return harness;
}

} // namespace a11y_uia_host_test
