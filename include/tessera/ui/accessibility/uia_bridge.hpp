// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/accessibility/uia_apartment.hpp"
#include "tessera/ui/accessibility/uia_error.hpp"
#include "tessera/ui/accessibility/uia_handoff.hpp"
#include "tessera/ui/accessibility/uia_provider.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace tessera::ui::a11y {

// The bridge owns one dedicated MTA thread that serves the UIA provider surface
// (docs/specs/accessibility-v1.md section 7, docs/design/accessibility.md section 3, ADR-021).
//
// - The UI thread only calls `publish` (bounded snapshot handoff) and `take_actions` (bounded ring
//   drain). It never calls UIA, never touches COM, and never waits on the bridge thread.
// - The bridge thread owns the COM apartment and the provider; every client call is serviced
//   there. A client call from any other thread is marshaled with `run_on_bridge` (bounded wait).
// - Actions never run on the bridge thread; the provider only queues validated requests.
//
// The apartment is the platform seam: Windows initializes `COINIT_MULTITHREADED` and pumps its
// message loop; every other platform's apartment reports `UnsupportedPlatform`, so the bridge
// starts and immediately reports accessibility disabled instead of failing the app.
struct UiaBridgeConfig {
    // Upper bound of one idle wait; the loop never spins. The bridge only does work when a client
    // call or an explicit `step` wakes it, so this is also the message-pump latency.
    int idle_wait_ms = 16;
    // Liveness bound for `stop()` before it records `Timeout`; the join still completes because
    // every iteration (and every job) is bounded.
    int shutdown_wait_ms = 2000;
    // Jobs one loop iteration may run before yielding, keeping a single pump bounded.
    std::size_t max_jobs_per_iteration = 4;
    // Bounded cross-thread job ring; a client call beyond it gets `QueueFull`.
    std::size_t max_queued_jobs = 8;
};

// Lifecycle hooks that run on the bridge thread, so the host can create the UIA root provider
// after the COM apartment exists and release it while the apartment is still alive. `on_start`
// runs once after `init` succeeds and before `phase()` turns Running; a throwing or failing hook
// fails the bridge with `Internal` and no root is published. `on_stop` runs once after the loop
// exits and before `apartment->shutdown()`; it must not block. Both must be set before `start()`
// and must not call back into `UiaBridge::stop`.
struct UiaBridgeHooks {
    std::function<void(UiaProvider&)> on_start;
    std::function<void(UiaProvider&)> on_stop;
};

enum class UiaBridgePhase : std::uint8_t {
    Idle,     // constructed, never started
    Starting, // worker spawned, apartment not initialized yet
    Running,  // apartment initialized, provider served
    Failed,   // terminal: `error()` says why (apartment, thread, or provider init)
    Stopped,  // joined after `stop()`
};

[[nodiscard]] std::string_view to_string(UiaBridgePhase phase);

class UiaBridge {
public:
    using ProviderFn = std::function<void(UiaProvider&)>;

    // `apartment` overrides the platform seam (tests inject a fake COM apartment).
    explicit UiaBridge(UiaBridgeConfig config = {}, std::unique_ptr<UiaApartment> apartment = {});
    ~UiaBridge();

    UiaBridge(const UiaBridge&) = delete;
    UiaBridge& operator=(const UiaBridge&) = delete;

    // Spawns the one bridge thread. Non-blocking: the apartment initializes on that thread and
    // `phase()` moves Starting -> Running (or Failed with `error()`). Idempotent while alive.
    [[nodiscard]] std::expected<void, UiaError> start();
    // Signals the loop and joins the thread. Safe to call twice, from the destructor, or when never
    // started. `phase()` becomes Stopped unless the worker already failed.
    void stop();
    [[nodiscard]] bool started() const;
    [[nodiscard]] bool running() const;
    [[nodiscard]] UiaBridgePhase phase() const;
    [[nodiscard]] UiaError error() const;
    [[nodiscard]] std::thread::id bridge_thread() const;
    [[nodiscard]] const UiaBridgeConfig& config() const;

    // Bridge-thread lifecycle hooks (see UiaBridgeHooks). Set them before `start()`; calling
    // again while the worker is alive is ignored so a running bridge never swaps hooks.
    void set_hooks(UiaBridgeHooks hooks);

    // UI thread: publish the latest snapshot. Drop-oldest with a counter; never blocks on the
    // bridge. The snapshot is adopted on the next client call or `step`.
    void publish(const A11ySnapshot& snapshot);
    [[nodiscard]] SnapshotHandoffStats handoff_stats() const;
    // UI thread: drain actions queued by the provider, to execute at a tick boundary.
    [[nodiscard]] std::size_t take_actions(std::span<UiaActionRequest> out);

    // Bridge thread: the provider over the current snapshot.
    [[nodiscard]] UiaProvider& provider();

    // Runs `fn(provider)` on the bridge thread, inline when the caller is already there. The wait
    // is a liveness bound, not a cancellation point: `fn` must not capture data whose lifetime
    // ends before it returns. Never call it from the UI thread.
    [[nodiscard]] std::expected<void, UiaError> run_on_bridge(const ProviderFn& fn,
                                                              int wait_ms = 5000);
    // Deterministic driver/barrier: adopts a pending snapshot and runs one loop iteration.
    // Tests and hosts with their own step-per-call driver use it; the UI thread never does.
    [[nodiscard]] std::expected<void, UiaError> step(int wait_ms = 5000);

private:
    struct Job {
        ProviderFn fn;
        std::uint64_t ticket = 0;
        bool done = false;
        bool running = false;
        bool abandoned = false;
        UiaError error = UiaError::None;
    };

    void run_loop();
    void run_jobs();
    void finish_worker(UiaError failure);
    [[nodiscard]] std::expected<void, UiaError> submit(const ProviderFn& fn, int wait_ms);
    [[nodiscard]] int idle_wait_ms() const;

    UiaBridgeConfig config_;
    std::unique_ptr<UiaApartment> apartment_;
    UiaProvider provider_;
    SnapshotHandoff handoff_;
    UiaBridgeHooks hooks_;
    // Reused fetch buffer: `take` copies into it and `set_snapshot` swaps its capacity back, so a
    // steady-state drain does not allocate.
    A11ySnapshot fetch_;

    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    // Fixed-capacity FIFO of in-flight client jobs. Jobs are shared with their submitters so a
    // completed job's error can be read after its slot is popped.
    std::vector<std::shared_ptr<Job>> jobs_;
    std::size_t job_head_ = 0;
    std::size_t job_count_ = 0;
    std::uint64_t next_ticket_ = 0;
    std::thread::id bridge_thread_{};
    bool stop_requested_ = false;
    bool worker_done_ = false;

    std::atomic<UiaBridgePhase> phase_{UiaBridgePhase::Idle};
    std::atomic<UiaError> error_{UiaError::None};
    std::atomic<bool> alive_{false};
};

} // namespace tessera::ui::a11y
