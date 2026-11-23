// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <memory>

namespace tessera::hal {

// The cross-thread wake a parked UI thread joins (issue #842). A completion posted from a worker
// has to end the idle park early, or an async result waits out the host's clamp;
// `docs/design/hal.md` section 1.1 promises exactly that.
//
// The interface is OS-free (ADR-020): a platform's `wait_for_events` reads the descriptor or the
// OS event it knows, and every other consumer uses `wait_until`. Nothing here runs a completion or
// calls user code; the wait only ends, and the next tick drains the queue on the UI thread.
class WakeSource {
public:
    virtual ~WakeSource() = default;

    // Signals the wake. Called from ANY thread, including a worker that just posted a completion.
    // Idempotent: signalling an already-signalled wake stays signalled, so a burst is one wake.
    virtual void wake() noexcept = 0;

    // Clears a signalled wake so the next wait blocks again. Waiting (UI) thread only.
    virtual void drain() noexcept = 0;

    // Blocks the calling thread until `wake()` or `deadline_seconds` (steady clock) and returns
    // true when woken. Bounded: a past or non-finite deadline returns immediately. UI thread only.
    [[nodiscard]] virtual bool wait_until(double deadline_seconds) = 0;

    // The `poll()` descriptor a platform wait adds beside its display fd, or -1 when the platform
    // waits on an OS event instead (Win32).
    [[nodiscard]] virtual int file_descriptor() const noexcept {
        return -1;
    }

    // The Win32 event handle a message wait adds to its handle list, or nullptr (POSIX).
    [[nodiscard]] virtual void* os_event() const noexcept {
        return nullptr;
    }
};

// Creates the platform's wake primitive, or nullptr where the platform wait has nothing to join
// (a build with no window backend, or Android's advisory lane). A null answer keeps today's
// bounded-sleep behaviour byte for byte.
[[nodiscard]] std::unique_ptr<WakeSource> create_wake_source();

// An in-process wake for tests and headless runs (issue #842): a mutex and a condition variable,
// with no descriptor. `file_descriptor()` and `os_event()` are null, so a platform wait cannot join
// it; `wait_until` is what a window-less loop uses. Deterministic: a `wake()` before a `wait_until`
// is remembered, so the wait returns immediately rather than racing the signal.
[[nodiscard]] std::unique_ptr<WakeSource> create_manual_wake_source();

} // namespace tessera::hal
