# Threading, ownership, and memory

Status: design for Big Step B2/B3 (runtime slice and window/swapchain phases of [Big Step B](../../AGENTS.md#big-steps-group-work-dont-micro-step)). The B1 runtime is synchronous and offscreen. Normative
details: [`memory-model-v1.md`](../specs/memory-model-v1.md) and [`bridges.md`](bridges.md).

Tessera must stay responsive while doing IO, decoding, shaping, and GPU uploads. The rules
below are normative; [ADR-021](../../decisions/ADR-021-async-runtime.md) (async runtime: "never block the UI thread") is the top-level constraint.

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

Custom elements ([`custom-elements-v1.md`](../specs/custom-elements-v1.md)): `on_update`/`on_event` run on the UI thread; `on_render` runs on the render
thread. They exchange immutable frame data/commands, not mutable UI state. Per-instance renderer
state is owned by the element; shared pipelines/meshes/textures live in the resource cache and
retire after frames in flight. No locks/blocking in the steady-state path.

## 2. Executors and async

- C++20 coroutines with an executor abstraction (`ui`, `worker`, `io`, `gpu`).
- Tasks return `Task<T>`/`std::expected`; cancellation is a token, not a detached thread.
- Completion queues are bounded; overflow is a documented error, not an OOM.
- UI completions are drained at a per-tick budget (`asyncMainThreadBudgetMs`, default 2.0 ms; see [`project-config-v1.md`](../specs/project-config-v1.md)).
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
- `std::pmr` (C++17 polymorphic memory resources) for containers whose locality matters, behind project aliases; no list of such containers exists yet.
- Avoid `std::function` in hot paths; use function pointers/small callables or type-erased
  interfaces with stable allocation.
- Strings: intern ids/tokens; avoid formatting in hot paths.

## 5. GPU memory

- VMA (Vulkan Memory Allocator) sub-allocates buffers/images; no raw `vkAllocateMemory` in the backend.
- Host-visible staging for uploads; device-local for sampled/attachment resources.
- Uploads are batched; no per-draw staging allocations.
- Frames in flight: 1 in B1, default 2 in B2/B3 (`gpuFramesInFlight`, range 1..3, see [`project-config-v1.md`](../specs/project-config-v1.md)); retire resources only after their fence/
  timeline value (Vulkan GPU-completion signals) completes.
- Caches (glyph atlas, textures, pipelines) are bounded with LRU/age eviction.
- Budgets: track bytes by category (vertex, texture, staging, atlas, targets).

## 6. Synchronization

- No locks in the steady-state UI hot path.
- Cross-thread communication uses bounded queues (SPSC/MPSC = single-producer/multi-producer, single-consumer) and immutable snapshots.
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
- Resize/swapchain: keep old target until in-flight frames retire ([`window-resize.md`](window-resize.md)).
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

- TSan/ASan/UBSan (thread, address, undefined-behavior sanitizers) builds for async/ownership paths.
- Cancellation/shutdown/queue-overflow tests.
- No-blocking checks: no `vkDeviceWaitIdle`/indefinite waits on the UI thread.
- Deterministic executor tests: same task order across runs.
- Allocation counters: steady-state frame does not allocate after warm-up.
- Resource retirement tests: in-flight frames keep resources alive; retirement releases them.

## 11. Large types accepted as they are (issue #731)

Issue #731 swept the tree for large structs. The ones that were fixed are pinned by
`padding_size_tests`, `paint_op_size_tests`, `background_layer_size_tests` and the `enforce` rows of
[`tests/golden/perf/memory_budget.json`](../../tests/golden/perf/memory_budget.json). The rest are
**accepted**, and this is the record of why. Each reason is the measured composition, not a guess.

| Type | sizeof | Why it stays |
|---|---|---|
| `hal::SyntheticEventInjector` | 33728 | Two fixed rings: `array<OsEvent, 64>` is 28160 and `array<InputEvent, 16>` is 5120. It is a **test and codegen-tool fixture only** — no `src/` translation unit references it, so it is never on a frame path. The audit's "33 KB move" framing is also wrong: the `make_*_script()` factories return by value and the return is elided (measured 0 moves, 0 copies). The real cost is default construction, 2.1 µs zeroing 33 KB, once per script. Shrinking it would mean `std::vector` rings, which buys nothing for a fixture and costs an allocation per script. |
| `ui::a11y::UiaHost` | 10408 | Holds two `A11ySnapshot`s (24 each) plus a `UiaEventQueue` (4688) and the bridge state. It is ticked only when dirty — `tick()` returns immediately unless `dirty_` is set — so a static window does no work. The buffers are reused across publishes precisely so a steady-state publish allocates nothing; making them smaller would trade a one-time 10 KB for per-frame allocation. |
| `ui::a11y::AtspiBridge` | 10200 | Same shape and same argument as `UiaHost`: dirty-gated, buffers reused, off the frame path when accessibility is idle. |
| `ui::a11y::EventQueue` | 9032 | `array<A11yEvent, 64>` at 140 bytes each. The bound is the coalescing contract (a bounded queue that never grows), so the size *is* the policy. |
| `ui::a11y::AtspiSession` | 4736 | Carries a 4096-byte `read_buffer_` inline, which is what keeps the D-Bus read path allocation-free. Moving it to the heap would allocate on every session, which is the opposite of the goal. |
| `ui::InputPump` | 6208 | Owns the input queues; the size is the queue bound, not padding. |
| `ui::InputRouter` | 3320 | `KeyBindingRegistry` (2576) inline. One router per window, not per node. |

The rule that follows: a large type is a problem when it is **per node** or **per frame**. Every row
above is per window, per session, or per test script, so the node count and the frame count do not
multiply it. The types that were fixed (`PaintOp`, `BackgroundLayerList`, `PaintData`, `Node`) are
exactly the ones that are per node or per painted node.
