# Async/await runtime and non-blocking rules

Status: implementation started for Big Step B2 (runtime slice) and design for B3 (window/swapchain
path); both are defined in [AGENTS.md Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step).
`ui/runtime/async.hpp` provides cancellation tokens/sources and a bounded deterministic
`ManualExecutor` FIFO with stats, plus a bounded `CompletionQueue` with visible/layout/background
priorities and a per-tick task budget. Cancellation tokens/sources also support once-only
`on_cancel` callbacks (immediate when already cancelled). `RuntimeLoop` drains the queue at the tick boundary, reports
the completion count, and keeps a wake pending until the queue is empty. Coroutine/executor
infrastructure is not wired yet. Normative decision:
[ADR-021](../../decisions/ADR-021-async-runtime.md) (async runtime; no UI-thread blocking). Terms: tick =
one fixed-step update of the runtime loop ([`game-loop.md`](game-loop.md)); golden = deterministic
reference image; V1 = first release, V1.1 = the release after V1 (as defined in
[`stylesheet-v1.md`](../specs/stylesheet-v1.md) line 8); see also
[`glossary.md`](../reference/glossary.md). Resource loading lifecycle:
[`resource-lifecycle-v1.md`](../specs/resource-lifecycle-v1.md).

## 1. Goals

- The UI thread never blocks on I/O, decoding, network, GPU waits, swapchain acquire/present, or
  a synchronous cross-thread call.
- Async work is structured, cancellable, observable, and deterministic in tests.
- Completions become **wake sources** for the fixed-step loop; no arbitrary-thread UI mutation.
- CPU-light work stays synchronous; async is not a default for everything.

## 2. Model

C++20 coroutines with an explicit executor/affinity model:

```cpp
template <typename T>
class Task;                      // lazy or eager, single-consumer

class Executor;                  // post/invoke
class MainThreadExecutor;        // resumes on the UI thread
class WorkerPool;                // CPU-heavy work
class IoExecutor;                // file/network waits

Task<Image> load_image(Path path, CancellationToken token);
Task<void>   upload(Image image, CancellationToken token);

Task<void> load_card(Card& card) {
    auto image = co_await load_image(card.path, card.cancel_token());
    co_await upload(std::move(image), card.cancel_token());
    card.mark_dirty();            // main-thread continuation
}
```

Rules:

- `co_await` must resume on the declared executor; UI-affine continuations use
  `MainThreadExecutor`.
- No detached fire-and-forget tasks. Every task has an owner/cancel scope.
- `sync_wait`/`future::get` are allowed only in tests/tools, never on the UI thread.
- Do not use `std::async`/`std::future` as the primary model: no cancellation, no affinity,
  blocking waits.
- `when_all`, `when_any`, `timeout`, and cancellation are first-class.
- Errors propagate as `Result<T, Error>` or a documented exception boundary ([`error-boundaries-v1.md`](../specs/error-boundaries-v1.md)); no exceptions across
  a C ABI or platform callback.
- Task names/ids are captured for tracing.

## 3. Executors and threading

| Executor | Runs | Examples |
|---|---|---|
| Main/UI | input, style, layout, scene mutation | completion continuations, `mark_dirty` |
| Worker pool | CPU-heavy, no UI state | image decode, text shaping (not in V1; see section 10), compression |
| I/O | file/socket waits | asset read, network request |
| GPU scheduler | command submission/fence wait | upload queue, target recreation (state machine) |

- UI state is UI-thread-owned. Workers receive immutable snapshots or owned buffers and return
  results; they never mutate the scene graph.
- A render thread is not part of V1 (see section 10); if added, it owns command recording; resource creation is posted to it.
- No locks in the steady-state hot path; use SPSC queues and message passing.
- Bounded queues: apply backpressure, coalesce, or cancel; never grow without limit.

## 4. What is async, what is not

| Work | Model | Why |
|---|---|---|
| File read/write | `Task` on I/O | may block |
| Image/font decode | `Task` on worker | CPU-heavy |
| Network | `Task` on I/O | may block |
| Shader compile (offline) | build step | not runtime |
| GPU upload | queue + scheduler event | no blocking wait |
| GPU fence/timeline wait | scheduler event | no UI-thread wait |
| Swapchain acquire/present | bounded state machine | may block/timeout |
| Window resize | event + state machine | compositor-driven |
| Style resolution | synchronous UI | small, deterministic |
| Layout | synchronous UI in V1 | Yoga is CPU-bound; async layout from a snapshot is not planned for V1 |
| Paint/draw-list build | synchronous UI in V1 | cacheable |
| Animation | fixed tick | deterministic |
| Dialogs | `Task` | user wait |

Async/await is not a substitute for a state machine. Resize/swapchain/device-loss paths are
event-driven state machines with explicit states, not a coroutine that parks the UI thread.

## 5. Integration with the loop

```text
tick:
  drain platform input queue
  drain async completions -> main-thread queue (bounded)
  apply UI mutations / mark dirty
  update -> style -> layout -> paint
  render if dirty/continuous
  request wake for pending async
```

- Async completion posts a message to the main queue; the loop drains it at a tick boundary.
- A pending task is a wake source only when it can produce visible work; background cancellations
  do not force frames.
- Per-tick main-thread completion budget (`asyncMainThreadBudgetMs`, default 2.0 ms in
  [`runtime-config-v1.md`](../specs/runtime-config-v1.md)); excess continuations are deferred to
  the next tick, with priority order:
  1. input/visible resources
  2. layout/paint-critical data
  3. background prefetch
- Cancellation is checked at await points and before committing results.
- Shutdown cancels scopes, drains bounded queues, and waits with a timeout; no infinite join.

## 6. GPU waits and present

- Never call `vkWaitForFences`/`vkDeviceWaitIdle`/blocking acquire/present on the UI thread.
- The frame scheduler owns fences/timeline semaphores. An async GPU task awaits a scheduler event
  that is resolved when the device signals completion.
- Acquire/present use timeouts and explicit states:
  `Acquire -> Record -> Submit -> Present -> Retire`.
- Out-of-date/suboptimal/device-lost transitions are handled by the same state machine as resize.
- Readback is for tests/tools only, on a worker, never in the frame loop.

## 7. Determinism and tests

- Deterministic tests use a manual executor with a stable FIFO order and simulated time.
- Golden mode performs all required resource loads before the first tick, or uses a deterministic
  scheduler; no background thread may reorder visible work.
- Task completion order is part of the test contract.
- Traces include task ids/spans so a slow or blocked task is visible.
- Fuzz/soak tests: cancel storms, queue overflow, shutdown during load, device loss during upload.

## 8. Diagnostics

- Task counters: created/completed/cancelled/failed, queue depth, wait time, run time.
- Trace spans for task start/end/await/resume; correlation with frame/tick ids.
- Main-thread blocking detector (debug): asserts no blocking wait on the UI thread.
- Leak detector: outstanding tasks/scopes at shutdown.
- See [`diagnostics.md`](diagnostics.md).

## 9. Anti-patterns

- `future.get()`/`std::async` on the UI thread.
- Detached tasks mutating UI state from a worker.
- Unbounded task queues or "async everything".
- Blocking acquire/present/device waits in the frame loop.
- Using a coroutine to hide a state machine that needs explicit cancellation/rollback.
- Async layout/paint without immutable snapshots and generation checks.

## 10. Open questions

- Lazy vs eager `Task` and the exact allocator strategy for coroutine frames.
- Whether text shaping moves to the worker pool in V1.1 (no decision yet; see [`text.md`](text.md)).
- Render-thread ownership and command recording.
- Priority inversion between UI completions and background prefetch.
- Structured-concurrency library vs a small in-house implementation.
