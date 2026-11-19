// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::rendering::rhi {

// The one question a retire queue asks a backend: "has the GPU reached this completion value?"
// (ADR-053 decision 2, docs/design/gpu/metal-backend.md section 4).
//
// Every backend already answers it in its own vocabulary - a Vulkan timeline semaphore counter
// query, a Metal `MTLSharedEvent.signaledValue` load - and the retire rule built on top is
// identical: a resource is destroyed or reused only after the value of its last submission is
// reached. This interface is that shared seam, so the rule has one implementation instead of one
// per backend.
//
// Contract:
//  - `completed_value()` is a non-blocking host query. It never waits, never submits and never
//    allocates; the frame path calls it every frame.
//  - Values are monotonic: a value that was reached stays reached, and a query never reports a
//    value the GPU has not reached.
//  - A value handed out for a submission that never reached the queue is skipped, never reused
//    (docs/design/gpu/metal-backend.md section 3.1).
//  - The implementation is called from the thread that drains the retire queue. A backend whose
//    completion is published by a GPU-chosen thread (Metal's completed handler) publishes into an
//    atomic there and answers this query from it; it does not free anything on that thread.
class CompletionSource {
public:
    virtual ~CompletionSource() = default;

    [[nodiscard]] virtual std::uint64_t completed_value() const = 0;
};

} // namespace tessera::rendering::rhi
