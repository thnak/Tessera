# Frame execution and synchronization

Status: B1 synchronous path implemented; `Batched2DRenderer` frame-slot (`frames_in_flight`)
support and the two-frame swapchain submit ring are implemented; a device-level frame object with
timeline semaphores is still proposed. Frame pacing policy is in
[`../game-loop.md`](../game-loop.md); dirty work in [`../dirty-rendering.md`](../dirty-rendering.md);
resize in [`../window-resize.md`](../window-resize.md).

## 1. Terms

| Term | Meaning |
|---|---|
| Frame slot | CPU/GPU state for one in-flight frame |
| Frames in flight | Submitted frames not yet completed (`gpuFramesInFlight`, 1..3) |
| Completion value | Fence or timeline-semaphore value for a submitted frame |
| Pass | Ordered GPU work with declared reads/writes |
| Retire | Destroy/reuse a resource after its last in-flight use |

## 2. Lifecycle

```text
begin_frame
  -> acquire a frame slot (wait only if all slots are busy)
  -> apply uploads/completions for this slot
  -> record passes
  -> end_frame / submit
  -> completion (fence/timeline)
  -> retire old resources
  -> reuse slot
```

B1 blocking path: one slot, synchronous `submit` (offscreen, readback, goldens). B2: 2 slots by
default (config 1..3), CPU records the next frame while the GPU executes the previous one. B3 adds
swapchain acquire/present around the same lifecycle.

### Current implementation

The frame-slot idea is implemented by `Batched2DRenderer`, not yet by a device-level frame object:

- `Batched2DConfig::frames_in_flight` (default 1) sets the number of vertex-buffer slots used
  round-robin, one per frame.
- **Caller contract:** with N slots, frame k + N must not begin until frame k's command list is
  complete (`is_complete()` or a bounded `wait()`), because it rewrites frame k's slot. With the
  default of 1, finish each frame (blocking `submit()` or a wait) before beginning the next.
  Nothing enforces this at runtime, and synchronization validation cannot detect a host write
  racing a GPU vertex read, so it is covered only by the renderer's recording-device test.
- Within one frame, flushes append at increasing offsets and draw with a base vertex, so a mid-frame
  flush (for example a viewport change) never overwrites vertices that earlier draws still read.
  When a slot's buffer is full, a larger one is added; buffers already referenced by recorded
  draws are never destroyed until the slot is next reused.
- Reference loop: the windowed mode of `tools/poc/batched2d` uses two slots, `submit_async`, a
  bounded wait on the slot's previous list (a timeout skips the frame), and one `wait_idle` at
  shutdown. This is the shape a real loop should follow.

## 3. Frame slots

- A slot owns command buffers, staging ring, descriptor state, and completion state.
- Never record into a slot whose completion value is not reached.
- If all slots are busy, wait for the oldest with a bounded timeout; do not allocate unbounded
  slots or spin.
- The number of slots is fixed at device creation; changing it recreates the device/runtime.
- Completion order is FIFO per queue in V1; cross-queue dependencies are deferred.
- A stalled GPU must not grow CPU memory: uploads/pending work use bounded queues.

## 4. Pass plan

V1 uses an explicit pass list, not a general render graph:

1. **Upload/copy** — staging to textures/buffers; no UI draws.
2. **Offscreen 2D** — clear/load target; batched quads; custom elements composited or recorded.
3. **Custom element passes** — offscreen targets (default) or inline pass (advanced).
4. **Composite** — offscreen element targets into the frame target.
5. **Readback/present** — copy to readback buffer (tests) or present (B3).

Rules:

- Pass order is deterministic and owned by the renderer, not by component tree order.
- Each pass declares color/depth attachments and read/write resources.
- A pass may not read a resource it writes in the same pass unless explicitly allowed.
- No pass creates pipelines/resources in the steady state.
- A general render graph (automatic pass culling/reordering) is deferred; it is not needed for
  the ordered 2D UI path.

## 5. Synchronization and barriers

- The backend owns image layouts, queue ownership, and API barriers for its own resources.
- The RHI exposes no raw barriers in B1; callers declare usage through descriptors/passes.
- No implicit barrier may be inserted at draw time; transitions happen at pass boundaries or
  upload/copy boundaries.
- Uploads complete before sampling; target transitions happen before rendering and before copy.
- Frames in flight use per-slot fences/timeline values; resources carry the last value that uses
  them and retire after that value completes.
- The UI thread never waits on a GPU fence in the steady state; waits happen on the render thread
  or through the scheduler with a bounded timeout.
- Deterministic mode waits after each frame; no overlap.

## 6. Uploads and readbacks

- Target: uploads use a per-slot staging ring; no per-texture command list or submit. Today each
  texture upload still records and submits its own list, but asynchronously, with its staging
  buffer retired after completion (see [backends.md](backends.md#3-vulkan-13)).
- Upload work is batched before drawing; the first use of a resource is ordered after its copy.
- Readback uses a `Readback` buffer; mapping is bounded and never on the UI hot path.
- Readback for tests is explicit (`copy_target_to_buffer` + completion); production screenshots
  use the same path with a size/frequency limit.
- Pending uploads/readbacks are cancelled or drained deterministically at shutdown.

## 7. Resize, DPI, and swapchain

- B1 offscreen targets are recreated on size change; old targets retire after in-flight frames.
- B3 keeps the old swapchain images until acquired frames retire; never destroy an acquired
  image.
- DPI/scale changes recreate targets/pipelines only where formats/sizes require it.
- Resize does not reorder UI state or drop queued input; rendering catches up at the new size.
- A failed acquire/present is a normal state (out-of-date) and recreates the swapchain.

## 8. Failure and recovery

| Failure | Response |
|---|---|
| Device lost | Stop submissions, preserve UI state, recreate device/resources, report |
| Out of memory | Evict caches, drop optional targets, retry once, then report |
| Queue/fence timeout | Treat as device lost; no infinite wait |
| Invalid command | Validation error; fail test/debug; fallback in release |
| Swapchain out-of-date | Recreate; skip present this frame |
| Shader/pipeline failure | Fallback pipeline or error placeholder; never crash the frame loop |

Recovery is explicit and observable; it never silently changes scroll/focus/selection state.

## 9. Determinism and tests

- Fixed frame count, fixed dt, fixed target size, no real-GPU baselines.
- Completion values are injected in unit tests; no sleeping for fences.
- Tests: slot reuse, bounded wait, upload before use, readback after copy, resize retirement,
  device-loss path, OOM eviction, pass order.
- Golden frames use the software/lavapipe path; real GPUs are smoke/tolerance only.

## 10. Deferred/open

- Cross-queue synchronization and async compute.
- General render graph/culling/reordering.
- Timeline-semaphore sharing across devices.
- Variable-rate shading and foveation.
- Multi-window/multi-swapchain frame scheduling.
