# Frame execution and synchronization

Status: B1 synchronous path implemented; `Batched2DRenderer` frame-slot (`frames_in_flight`)
support and the two-frame swapchain submit ring are implemented, and the swapchain path now paces
those frames with one timeline semaphore (section 5.1). Frame pacing policy is in
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
  shutdown. This is the shape a real loop should follow. The production `WindowHost` loop instead
  gates the swapchain slots on the timeline counter (section 5.1) and never waits.

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
  them and retire after that value completes. The swapchain path uses one timeline semaphore, see
  section 5.1.
- The UI thread never waits on a GPU fence in the steady state; waits happen on the render thread
  or through the scheduler with a bounded timeout.
- Deterministic mode waits after each frame; no overlap.

### 5.1 Swapchain frame pacing (timeline semaphore)

The window/swapchain path owns one timeline semaphore per device generation
(`VulkanSwapchain::timeline_`). Everything below is implemented in
`src/rendering/backend/vulkan/src/vulkan_swapchain*.cpp` and `vulkan_command_list.cpp`; the
offscreen/readback path deliberately keeps its blocking fence submit, because offscreen work is
allowed to wait (ADR-021 is about the UI thread).

- **Creation.** The device enables `VkPhysicalDeviceVulkan12Features::timelineSemaphore` when it is
  created (`vulkan_instance.cpp`) and fails device creation loudly if the physical device does not
  report it: Vulkan 1.3 mandates the feature, so a device without it is broken, not a reason to fall
  back silently. The semaphore itself is created with
  `VkSemaphoreTypeCreateInfo{VK_SEMAPHORE_TYPE_TIMELINE, initialValue = 0}` at the first swapchain
  creation for that device generation.
- **Values per submit.** An acquire hands the frame the next value (`timeline_next_`); the render
  submission that targets the acquired image waits on the slot's binary acquire semaphore and
  signals that timeline value through `VkTimelineSemaphoreSubmitInfo`. One painted frame consumes
  exactly one value. A value handed out for a submission that never reaches the queue is skipped
  rather than reused, which timeline semaphores allow because values only have to increase.
- **Slot reuse by counter query.** `vkGetSemaphoreCounterValue` reports the highest value the GPU
  reached. A slot whose value is reached is free; if neither of the two slots is free, acquire
  returns `SwapchainError::Timeout` and the UI thread skips that tick instead of waiting. The
  counter is also what `Swapchain::frame_sync()` samples (`timeline_value`, `frames_in_flight`,
  `slots_skipped`) and what `WindowFrameStats` reports.
- **No UI-thread wait.** There is no `vkWaitSemaphores`/`vkWaitForFences` in the per-frame path. The
  binary acquire semaphore stays per slot because image acquisition needs one, but it is waited on
  by the GPU inside the submission, never by the CPU. The per-image render-finished semaphore still
  orders present after rendering.
- **Recovery and teardown.** A new device generation (loss or recovery) creates a new timeline with
  the value bookkeeping reset to zero, and the old generation's handles stay destroyable under the
  existing epoch rules. Teardown and swapchain retirement drain with a bounded wait: the timeline
  wait for the last submitted value uses the same `kMaxWaitNs` (100 ms) bound as acquire/present,
  and an empty-submit fence still orders after a queued present, which the timeline value does not
  observe. Neither drain runs per frame.
- **What the smoke test asserts.** `tests/gpu/vulkan_timeline_frame_tests.cpp` (ctest
  `vulkan_timeline_frames`) pumps at least eight frames through `WindowHost::pump_once()` and checks
  that presents grew, the device stayed healthy, no frame failed, the timeline is monotonic, it
  never runs ahead of a submit, at most two submitted values were still in flight, at most two
  frames were ever in flight, and the Vulkan validation message counter is zero after teardown with
  synchronization validation enabled.

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
- Swapchain images are 1-sample; opt-in 4x presentation uses an offscreen 4x target + end-of-pass
  resolve and, until the RHI grows an image-to-image copy, a CPU readback -> texture -> fullscreen
  quad bridge (demo only; encoded-space/sRGB caveat documented in `window-resize.md` 7.1).

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
Device loss is proven end-to-end by the windowed POC (`--simulate-loss`) and offscreen by
`present_hardening`: frames stop, the caller releases every lost-generation RHI object, the
controller recovers on a later tick, and rendering resumes with validation on.

## 9. Determinism and tests

- Fixed frame count, fixed dt, fixed target size, no real-GPU baselines.
- Completion values are injected in unit tests; no sleeping for fences.
- Tests: slot reuse, bounded wait, upload before use, readback after copy, resize retirement,
  device-loss path, OOM eviction, pass order.
- Golden frames use the software/lavapipe path; real GPUs are smoke/tolerance only.
- `present_hardening` covers 3 deterministic frames with rounded clip + wrapped text + rotated quad,
  4x resolve readback when supported, and a simulated device-loss recovery cycle under validation.

## 10. Deferred/open

- Cross-queue synchronization and async compute.
- General render graph/culling/reordering.
- Timeline-semaphore sharing across devices.
- Variable-rate shading and foveation.
- Multi-window/multi-swapchain frame scheduling.
- Partial/incremental present and VRR: Win32 DXGI flip-model `Present1` dirty rects/scrolled areas
  and Vulkan `VK_KHR_incremental_present` are presentation-engine hints that need per-frame
  back-buffer coherence bookkeeping; X11 partial updates (`XPutImage`/`XShmPutImage`/`XCopyArea`)
  do not reduce compositor work and XDamage only reports. Damage stays a render-side work reduction
  (D4); VRR/partial present belongs to a later platform-capability slice.

- Render-side damage (D4, task-61): due frames with a small `RedrawPlan` are recorded through
  `begin_damaged` into a preserved target and presented full-frame; large/whole-screen plans fall
  back to full redraw. Offscreen `damage_present` proves pixel-identical output and reduced covered
  area; draw calls can stay the same (all batches intersect the region), so the win is raster area.
  Text/decoration/vector primitives damage their node content boxes; dirty-pass overflow
  under-damaging is tracked separately (task-65). Partial present/VRR remain deferred.

- Per-line text damage (task-66): `ui::layout::node_damage_rect` tightens text damage to the vertical
  union of emitted stub-metrics line boxes plus a 4 px ink/padding margin (full node width), with
  content-box fallbacks for alignment/decoration/unknown metrics; `damage_present` shows 9,840 ->
  6,720 px of text damage at tolerance 0. Draw calls stay equal to full frames (all batches
  intersect the region); no batch-splitting change was made because it cannot reduce draws without
  per-frame vertex filtering.

- POC damage probe (`--on-demand --damage-probe --validation`, task-66): prints per-frame redraw
  regions/area/%/full/draws/batches plus idle waits/presents; present remains full-frame into the
  swapchain, while preserved-target partial rendering is proven offscreen by `damage_present`.
