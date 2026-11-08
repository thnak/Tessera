# ADR-021: Async/await runtime; never block the UI thread

- Status: Proposed
- Date: 2026-11-07
- Related: [ADR-017](ADR-017-input-and-fixed-step-runtime.md) (input and fixed-step runtime loop), [ADR-018](ADR-018-dirty-rendering-and-frame-pacing.md) (dirty rendering and frame pacing), [ADR-020](ADR-020-window-resize-and-lifecycle.md) (window resize, display scale, and lifecycle), [ADR-022](ADR-022-diagnostics-and-profiling.md) (diagnostics and profiling)

## Context

Native I/O, image/font decoding, network, GPU uploads, fence waits, swapchain acquire/present,
and dialogs can block. A UI framework must keep the UI thread responsive and must not freeze on a
slow disk, GPU, compositor, or network. C++20 coroutines are available; `std::future`/`std::async`
lack cancellation, affinity, and structured lifetime.

## Decision

1. Use **C++20 coroutines** with a small `Task<T>` abstraction and explicit executors:
   main/UI, worker pool, I/O, and the GPU scheduler.
2. **No blocking on the UI thread**: no `future::get`, `std::async`, `vkWaitForFences`,
   `vkDeviceWaitIdle`, blocking acquire/present, synchronous cross-thread calls, or blocking I/O.
3. **Structured concurrency**: every task has an owner/cancel scope; no detached fire-and-forget
   tasks; `when_all`/`when_any`/`timeout` and cancellation are first-class.
4. UI-affine continuations resume on the main executor. Workers receive immutable snapshots/owned
   buffers and never mutate the [scene graph](../docs/reference/glossary.md).
5. Async completions post to a bounded main-thread queue and are drained at tick boundaries (a tick is one fixed-timestep simulation step, see decision 2 of [ADR-017](ADR-017-input-and-fixed-step-runtime.md)); they
   are wake sources for the loop. Per-tick completion budget with priorities.
6. **GPU waits** are scheduler events resolved when the device signals completion; the frame loop
   uses explicit acquire/record/submit/present/retire states, not blocking waits.
7. **Resize/device-loss/swapchain** paths are state machines, not coroutines that park the UI
   thread.
8. Shutdown cancels scopes, drains bounded queues, and waits with a timeout; no infinite join.
9. Deterministic tests use a manual executor with stable FIFO ordering and simulated time.
10. `sync_wait` is limited to tests/tools. Blocking-wait detection is enabled in debug.
11. Diagnostics: task ids/spans, queue depth, run/wait/cancel counts, leak detection.

## Consequences

### Positive

- Responsive UI under slow I/O/GPU/network.
- Deterministic, cancellable resource loading.
- No hidden thread-affinity bugs or detached-task leaks.
- Testable async ordering.

### Negative / risks

- Coroutine frame allocation/lifetime needs care.
- Executor/cancellation machinery is non-trivial.
- Async layout/paint must use snapshots and generation checks if introduced.
- More complex debugging; requires tracing/blocking detectors.

## Alternatives considered

- **`std::async`/`std::future` + `get()`.** Rejected: blocking, no cancellation/affinity.
- **Callback-only model.** Rejected: callback hell, lifetime/ownership errors, hard cancellation.
- **Synchronous everything.** Rejected: freezes on I/O/GPU/compositor.
- **Async everything.** Rejected: overhead/complexity for CPU-light style/layout/paint.
- **Fiber/coroutine framework.** Rejected for [V1](../docs/reference/glossary.md): too heavy; C++20 coroutines suffice.

## Follow-up

- Implement `Task<T>`, executors, cancellation, and the deterministic test executor.
- Implement the bounded completion queue and per-tick budget.
- Add GPU scheduler events for fence/timeline waits.
- Add blocking-wait detection and task tracing.
