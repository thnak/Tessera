// SPDX-License-Identifier: MIT

#pragma once

#include "completion_source.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace tessera::rendering::rhi {

// Deferred destruction keyed on completion values (ADR-053 decision 2,
// docs/design/gpu/metal-backend.md section 4, docs/design/gpu/rhi.md section 6).
//
// The rule this encodes is backend-independent: never destroy a resource the GPU may still be
// reading. A caller retires a destroyer against the completion value of the last submission that
// referenced the resource; the queue runs it once `CompletionSource::completed_value()` reaches
// that value. Vulkan and Metal share this implementation so the rule cannot drift between them.
//
// Threading: the queue is not thread-safe. It is pushed and drained from the thread that owns the
// device (the UI thread today, docs/design/gpu/rhi.md section 6). A backend whose completion is
// published from a GPU-chosen thread only stores an atomic there; it never touches this queue.
//
// Allocation: `retire()` may allocate (it is a teardown-path call, not a per-draw one). `reap()`
// allocates nothing in the steady state: it reuses its scratch buffer and only grows it when more
// entries are ready at once than ever before.
class RetireQueue {
public:
    // Runs `destroy` once `value` is reached. A value already reached runs it immediately, so a
    // caller never has to special-case "nothing in flight".
    void retire(std::uint64_t value, std::function<void()> destroy);

    // Runs every destroyer whose value the source has reached. Destroyers run after the ready set
    // is detached, so one that calls back into the queue (a texture whose teardown retires
    // something else) cannot invalidate the iteration.
    void reap(const CompletionSource& source);

    // Runs every destroyer regardless of completion. Only for teardown, after the caller has
    // waited for the device to go idle with a bound (docs/design/gpu/metal-backend.md section 7).
    void drain();

    [[nodiscard]] std::size_t pending() const {
        return entries_.size();
    }

    [[nodiscard]] bool empty() const {
        return entries_.empty();
    }

private:
    struct Entry {
        std::uint64_t value = 0;
        std::function<void()> destroy;
    };

    std::vector<Entry> entries_;
    // Reused across reap() calls so the steady state allocates nothing.
    std::vector<Entry> ready_;
};

} // namespace tessera::rendering::rhi
