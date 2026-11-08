# Async/await runtime and non-blocking rules

Status: implementation started for Big Step B2 (runtime slice) and design for B3 (window/swapchain
path); both are defined in [AGENTS.md Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step).
`ui/runtime/async.hpp` provides cancellation tokens/sources and a bounded deterministic
`ManualExecutor` FIFO with stats, plus a bounded thread-safe `CompletionQueue` with
visible/layout/background priorities and a per-tick task budget. Cancellation tokens/sources also
support once-only `on_cancel` callbacks (immediate when already cancelled). `WorkerExecutor`
(`runtime/worker_executor.hpp`) owns a bounded pool of worker threads, rejects work after
`shutdown()`, drains queued tasks, joins every thread, and can post completions back to the
`CompletionQueue` by priority. `RuntimeLoop` drains the queue at the tick boundary, reports
the completion count, and keeps a wake pending until the queue is empty. The coroutine slice
(`runtime/task.hpp`) adds a lazy `Task<T>`/`Task<void>` that resumes only through a
`CompletionQueue`, propagates results and exceptions at the `co_await` boundary, and checks
cancellation before every resume; an I/O executor is not implemented yet. The exact implemented
subset and its limitations are in [section 2.1](#21-implemented-coroutine-subset-b2). Normative
decision:
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

C++20 coroutines with an explicit executor/affinity model. The B2 slice implements the `Task` half
plus the queue/worker scheduling primitives; the named executors below are target design for B3+:

```cpp
template <typename T>
class Task;                      // lazy, single-consumer (implemented)

class Executor;                  // post/invoke (CompletionQueue today)
class MainThreadExecutor;        // resumes on the UI thread (target)
class WorkerPool;                // CPU-heavy work (WorkerExecutor today)
class IoExecutor;                // file/network waits (planned)

Task<Image> load_image(TaskContext ctx, Path path);
Task<void>   upload(TaskContext ctx, Image image);

Task<void> load_card(TaskContext ctx, Card& card) {
    auto image = co_await load_image(ctx, card.path);
    co_await upload(ctx, std::move(image));
    card.mark_dirty();            // queue-drained continuation
}
```

Rules:

- `co_await` must resume on the declared executor; UI-affine continuations use
  `MainThreadExecutor` (in B2, the `CompletionQueue` drained by `RuntimeLoop`).
- No detached fire-and-forget tasks. Every task has an owner/cancel scope.
- `sync_wait`/`future::get` are allowed only in tests/tools, never on the UI thread.
- Do not use `std::async`/`std::future` as the primary model: no cancellation, no affinity,
  blocking waits.
- `when_all`, `when_any`, `timeout`, and cancellation are first-class.
- Errors propagate as `Result<T, Error>` or a documented exception boundary ([`error-boundaries-v1.md`](../specs/error-boundaries-v1.md)); no exceptions across
  a C ABI or platform callback.
- Task names/ids are captured for tracing.

### 2.1 Implemented coroutine subset (B2)

`include/tessera/runtime/task.hpp` + `src/runtime/task.cpp` implement the following subset:

- **Lazy.** Calling a task coroutine creates the frame and returns a `Task<T>`/`Task<void>`; nothing
  runs until `start()` posts the first resume or another task `co_await`s it.
- **Context.** Every task coroutine takes `TaskContext` as its first parameter (completion queue,
  priority, cancellation token). A task without one does not compile; the queue and token must
  outlive the task.
- **Queue-only resumption.** `start()` and every continuation post a closure to the task's
  `CompletionQueue` at the priority from its context. A continuation is posted to the *awaiting*
  task's queue/priority, so completion never resumes inline and no thread is created, detached, or
  joined.
- **Results and errors.** `co_await` yields the value (or void) and rethrows the child's exception
  at the await point. An exception that leaves a task body is stored in the task state, so a root
  task fails without terminating.
- **Cancellation.** Checked inside the queued resume: before the first resume and before every
  later resume, including resumptions of the awaiting task. A cancelled task never runs its body
  (or the part after the await) and finishes with `TaskCancelled`; the awaiter sees it rethrown at
  `co_await`.
- **Single consumer.** A `Task` is awaited at most once; a second `co_await` throws
  `std::logic_error`. A second `start()` returns `false` and does not double-resume.
- **Lifetime.** An internal shared state owns the coroutine frame; an awaiting task keeps the
  frame alive while suspended, and the frame is destroyed exactly once, when the result is
  consumed or the last `Task`/awaiter is dropped.

Limitations of this slice:

- **No I/O executor and no thread affinity beyond the queue.** A resume runs on whichever thread
  drains the `CompletionQueue` (the UI thread in `RuntimeLoop`); nothing migrates or pins a task to
  a thread, and a worker thread must not touch UI state. One task graph must be pumped by one
  thread at a time: the queue is thread-safe, but task state assumes a single drainer.
- No `sync_wait`, `when_all`, `when_any`, `timeout`, task names/ids, or coroutine-frame allocator
  hooks yet (see [section 10](#10-open-questions)).
- Cancellation is observed only at resume boundaries. A task suspended on a child is cancelled
  when the child next completes; there is no eager cancellation wakeup.
- Dropping the last `Task` handle does not cancel or detach a graph: a started task still runs to
  its next completion on the queue, and a parent suspended on a child keeps both states alive until
  the child finishes (the reference cycle breaks at that point). Cancel the token to stop work.
- A full bounded queue drops the resume (`CompletionQueue::Stats::dropped`) and the task stays
  suspended; the caller sizes the queue and the per-tick budget.
- One `shared_ptr` control block per task frame; no frame pool yet.

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
- Tasks resume only from the queue, so a test pumps `CompletionQueue::run` and asserts the exact
  execution order and step count (`tests/unit/task_tests.cpp`); nothing runs on a timer or thread.
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

- Lazy vs eager `Task`: settled as lazy (B2); the coroutine-frame allocator/pool strategy is still
  open.
- Whether text shaping moves to the worker pool in V1.1 (no decision yet; see [`text.md`](text.md)).
- Render-thread ownership and command recording.
- Priority inversion between UI completions and background prefetch.
- Structured-concurrency library vs a small in-house implementation.
