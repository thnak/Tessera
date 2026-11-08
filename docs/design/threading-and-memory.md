# Threading, ownership, and memory

Status: design for Big Step B2/B3. The B1 runtime is synchronous and offscreen. Normative
details: [`memory-model-v1.md`](../specs/memory-model-v1.md) and [`bridges.md`](bridges.md).

Tessera must stay responsive while doing IO, decoding, shaping, and GPU uploads. The rules
below are normative; ADR-021 ("never block the UI thread") is the top-level constraint.

## 1. Threads

| Thread | Owns | May do | Must not do |
|---|---|---|---|
| UI/main | scene graph, style/layout/animation state, input, command recording | mutate UI state, record commands, drain completions | block on GPU/IO, take long locks |
| Worker pool | independent tasks | IO, decode, preprocessing, CPU shaping | touch scene nodes/window handles |
| IO | file/network waits | async reads/writes | call UI code directly |
| GPU/render (optional B3) | swapchain/presentation, GPU submission | consume immutable draw lists, present | mutate scene/style/layout |
| OS callbacks | platform events | enqueue normalized events | run user code/layout/render |

B1: one thread, synchronous submit/wait. B3 may add a render thread, but the UI thread still
owns scene/style/layout and never waits indefinitely.

## 2. Executors and async

- C++20 coroutines with an executor abstraction (`ui`, `worker`, `io`, `gpu`).
- Tasks return `Task<T>`/`std::expected`; cancellation is a token, not a detached thread.
- Completion queues are bounded; overflow is a documented error, not an OOM.
- UI completions are drained at a per-tick budget (`asyncMainThreadBudgetMs`).
- No detached tasks; every task has an owner and a shutdown path.
- Blocking calls are allowed only on dedicated IO/worker threads with timeouts.

See [`async.md`](async.md) and [`runtime-config-v1.md`](../specs/runtime-config-v1.md).

## 3. Ownership

- `std::unique_ptr` for ownership; `std::shared_ptr` only for genuinely shared lifetime.
- Non-owning views: `std::span`, `std::string_view`, stable handles/ids.
- Nodes own children; components own state; the device owns GPU resources.
- `useRef`/node handles are non-owning; clear them on unmount.
- Async callbacks capture stable ids/handles, never raw pointers to UI objects.
- Destruction order: detach/retire UI -> release GPU resources after frames in flight ->
  destroy device.

## 4. Allocators and hot paths

- No hidden allocations in per-frame steady state after warm-up.
- Frame arena for temporary per-frame data; reset at frame boundary.
- Object pools for nodes/commands where profiling shows churn.
- Ring buffers for streaming chart data and vertex batches.
- `std::pmr` where it improves locality, behind project aliases.
- Avoid `std::function` in hot paths; use function pointers/small callables or type-erased
  interfaces with stable allocation.
- Strings: intern ids/tokens; avoid formatting in hot paths.

## 5. GPU memory

- VMA sub-allocates buffers/images; no raw `vkAllocateMemory` in the backend.
- Host-visible staging for uploads; device-local for sampled/attachment resources.
- Uploads are batched; no per-draw staging allocations.
- Frames in flight: 1 in B1, default 2 in B2/B3; retire resources only after their fence/
  timeline value completes.
- Caches (glyph atlas, textures, pipelines) are bounded with LRU/age eviction.
- Budgets: track bytes by category (vertex, texture, staging, atlas, targets).

## 6. Synchronization

- No locks in the steady-state UI hot path.
- Cross-thread communication uses bounded queues (SPSC/MPSC) and immutable snapshots.
- Atomics for counters/flags; no busy-wait spins on the UI thread.
- `vkWaitForFences` only with a bounded timeout on a non-UI thread; the UI thread polls or
  waits on an event/fence via the scheduler.
- Avoid priority inversion: workers do not hold locks needed by the UI thread.
- Deterministic mode uses a single-threaded executor or a deterministic scheduler.

## 7. Frame lifetime

```text
begin frame -> acquire snapshot/commands -> record -> submit
  -> present -> completion -> retire old resources
```

- Draw lists/batches are immutable once submitted.
- Resources referenced by in-flight frames are not destroyed; use deferred deletion lists.
- Resize/swapchain: keep old target until in-flight frames retire (window-resize).
- Async uploads complete into a staging queue; the UI thread applies the completion.

## 8. Shutdown

- Stop accepting new tasks; cancel outstanding work with tokens.
- Drain/cancel completions without invoking UI callbacks after teardown.
- Wait for GPU work with a bounded timeout; then destroy resources/device.
- No detached threads; the process must exit deterministically.
- Crash/shutdown diagnostics record outstanding tasks and GPU resources.

## 9. Metrics

- UI-thread latency and long-task count.
- Worker/IO queue depth, task age, cancellations, overflow.
- Allocation counts/bytes in hot paths; frame arena high-water mark.
- GPU memory by category; staging upload bytes; cache hit/eviction.
- Frames in flight, fence wait time, deferred-deletion backlog.

## 10. Tests

- TSan/ASan/UBSan builds for async/ownership paths.
- Cancellation/shutdown/queue-overflow tests.
- No-blocking checks: no `vkDeviceWaitIdle`/indefinite waits on the UI thread.
- Deterministic executor tests: same task order across runs.
- Allocation counters: steady-state frame does not allocate after warm-up.
- Resource retirement tests: in-flight frames keep resources alive; retirement releases them.
