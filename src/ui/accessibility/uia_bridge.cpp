// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/uia_bridge.hpp"

#include <algorithm>
#include <chrono>
#include <system_error>
#include <utility>

namespace tessera::ui::a11y {

std::string_view to_string(UiaBridgePhase phase) {
    switch (phase) {
    case UiaBridgePhase::Idle:
        return "idle";
    case UiaBridgePhase::Starting:
        return "starting";
    case UiaBridgePhase::Running:
        return "running";
    case UiaBridgePhase::Failed:
        return "failed";
    case UiaBridgePhase::Stopped:
        return "stopped";
    }
    return "idle";
}

UiaBridge::UiaBridge(UiaBridgeConfig config, std::unique_ptr<UiaApartment> apartment)
    : config_(config),
      apartment_(apartment != nullptr ? std::move(apartment) : create_platform_apartment()) {
    jobs_.resize(std::max<std::size_t>(config_.max_queued_jobs, 1));
}

UiaBridge::~UiaBridge() {
    stop();
}

int UiaBridge::idle_wait_ms() const {
    // A bounded idle wait keeps the loop off the CPU without making message pumping unbounded.
    return std::clamp(config_.idle_wait_ms, 1, 1000);
}

bool UiaBridge::started() const {
    return alive_.load(std::memory_order_acquire);
}

bool UiaBridge::running() const {
    return phase() == UiaBridgePhase::Running;
}

UiaBridgePhase UiaBridge::phase() const {
    return phase_.load(std::memory_order_acquire);
}

UiaError UiaBridge::error() const {
    return error_.load(std::memory_order_acquire);
}

std::thread::id UiaBridge::bridge_thread() const {
    std::lock_guard lock(mutex_);
    return bridge_thread_;
}

const UiaBridgeConfig& UiaBridge::config() const {
    return config_;
}

void UiaBridge::publish(const A11ySnapshot& snapshot) {
    // Deliberately no wake-up: the worker adopts the snapshot at the start of the next client job
    // or `step`. That keeps a query-free bridge idle and makes the drop-oldest policy independent
    // of thread scheduling.
    handoff_.publish(snapshot);
}

SnapshotHandoffStats UiaBridge::handoff_stats() const {
    return handoff_.stats();
}

std::size_t UiaBridge::take_actions(std::span<UiaActionRequest> out) {
    return provider_.take_actions(out);
}

UiaProvider& UiaBridge::provider() {
    return provider_;
}

std::expected<void, UiaError> UiaBridge::start() {
    {
        std::lock_guard lock(mutex_);
        if (thread_.joinable()) {
            if (!worker_done_) {
                return {}; // idempotent while the worker is alive
            }
            thread_.join(); // finished worker: the join is already complete
        }
        stop_requested_ = false;
        worker_done_ = false;
        job_head_ = 0;
        job_count_ = 0;
        for (auto& job : jobs_) {
            job.reset();
        }
        phase_.store(UiaBridgePhase::Starting);
        error_.store(UiaError::None);
        try {
            thread_ = std::thread([this] { run_loop(); });
        } catch (const std::system_error&) {
            // Thread creation is the one OS call that can fail synchronously; report it as a value.
            worker_done_ = true;
            phase_.store(UiaBridgePhase::Failed);
            error_.store(UiaError::ThreadFailed);
            return std::unexpected(UiaError::ThreadFailed);
        }
        alive_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
    return {};
}

void UiaBridge::stop() {
    {
        std::lock_guard lock(mutex_);
        if (bridge_thread_ == std::this_thread::get_id()) {
            // A provider callback must never join its own thread; ask the loop to exit and let the
            // UI thread (or the destructor) join.
            stop_requested_ = true;
            cv_.notify_all();
            return;
        }
        if (!thread_.joinable()) {
            phase_.store(UiaBridgePhase::Stopped);
            return;
        }
        stop_requested_ = true;
    }
    cv_.notify_all();
    {
        std::unique_lock lock(mutex_);
        const auto bound = std::chrono::milliseconds(std::max(config_.shutdown_wait_ms, 0));
        if (!cv_.wait_for(lock, bound, [this] { return worker_done_; })) {
            // Diagnostic only: every loop iteration and every job is bounded, so the join below
            // still terminates; a host reading `Timeout` knows the advertised bound was exceeded.
            error_.store(UiaError::Timeout);
        }
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (phase() != UiaBridgePhase::Failed) {
        phase_.store(UiaBridgePhase::Stopped);
    }
    alive_.store(false, std::memory_order_release);
}

void UiaBridge::run_loop() {
    {
        std::lock_guard lock(mutex_);
        bridge_thread_ = std::this_thread::get_id();
    }
    provider_.adopt_thread(std::this_thread::get_id());

    const auto init = apartment_->init();
    if (!init) {
        // No shutdown: a failed init may not have balanced apartment state.
        finish_worker(init.error());
        return;
    }
    phase_.store(UiaBridgePhase::Running, std::memory_order_release);
    cv_.notify_all();

    for (;;) {
        std::size_t pending_jobs = 0;
        {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(idle_wait_ms()),
                         [this] { return stop_requested_ || job_count_ > 0; });
            if (stop_requested_) {
                break;
            }
            pending_jobs = job_count_;
        }
        if (pending_jobs > 0) {
            // Adopt the newest snapshot before serving the batch, so every call in this iteration
            // observes one consistent tree.
            if (handoff_.take(fetch_)) {
                provider_.set_snapshot(fetch_);
            }
            run_jobs();
        }
        apartment_->pump();
    }
    apartment_->shutdown();
    finish_worker(UiaError::None);
}

} // namespace tessera::ui::a11y
