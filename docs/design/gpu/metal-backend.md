# Metal backend: synchronization, resource lifetime and presentation

Status: design, nothing implemented. Decision: [ADR-053](../../../decisions/ADR-053-native-metal-backend.md)
(native Metal, no MoltenVK). Implementation issues: #204 (device, queue, resources), #205
(pipelines, MSL), #206 (`CAMetalLayer` window), #207 (conformance, Tier 2), #208 (NSAccessibility).
macOS and iOS are post-V1 ([AGENTS.md](../../../AGENTS.md#v1-platform-scope-and-ci-policy)), so this
is a contract to build against, not a V1 deliverable.

This document maps the RHI contract in [`rhi.md`](rhi.md) sections 5 to 7 and
[`frames.md`](frames.md) onto Metal. It does not change the contract. Where a Metal fact below is
marked **verify**, it must be confirmed on hardware before the slice that depends on it is merged.

## 1. What carries over unchanged

- Completion values, frame slots, "never destroy right after submit", generations, and the shutdown
  order are backend-independent rules. Metal implements them; it does not reinterpret them.
- The backend owns barriers and hazards. The RHI exposes no barrier calls (`rhi.md` section 5).
- The UI thread never waits without a bound (ADR-021). `submit()` is for offscreen, readback and
  goldens only.
- Pixel goldens come from lavapipe only; Metal is checked against `tolerance.json`
  ([ADR-009](../../../decisions/ADR-009-deterministic-headless-golden-tests.md)).

## 2. Object model

| RHI | Vulkan | Metal |
|---|---|---|
| `Device` | `VkDevice` + queue | `MTLDevice` + one `MTLCommandQueue` |
| `CommandList` | command buffer | `MTLCommandBuffer` |
| render pass | dynamic rendering | one `MTLRenderCommandEncoder` |
| `copy_*` | transfer commands | `MTLBlitCommandEncoder` between render encoders |
| completion value | timeline semaphore value | `MTLSharedEvent` signaled value |
| swapchain image | `VkImage` from the swapchain | `CAMetalDrawable` texture |

One queue in V1, matching "completion order is FIFO per queue" in `frames.md` section 3. Command
buffers committed to one `MTLCommandQueue` start in commit order.

## 3. Synchronization

### 3.1 Completion values

- The device owns one `MTLSharedEvent` and a monotonically increasing `uint64_t` counter, the same
  shape as the Vulkan timeline in `frames.md` section 5.1. Each submit takes the next value and
  encodes `encodeSignalEvent:value:` on its command buffer before commit. A value handed out for a
  submission that never reaches the queue is skipped, never reused.
- `is_complete()` is a lock-free load of the event's `signaledValue`. It does not block and does
  not take a mutex.
- `wait(timeout_ms)` uses `waitUntilSignaledValue:timeoutMS:` and returns whether the value was
  reached. **Verify** the minimum OS version for this call against the floor in section 9.
- `waitUntilCompleted` has no timeout and is never used on the frame path. It is allowed only in
  `submit()` (offscreen, readback, goldens).
- `addCompletedHandler:` runs on a thread Metal chooses. It may only touch atomics and the
  lock-free retire queue (section 4). It never touches scene state, never allocates on the frame
  path, and never throws. Completion is observed by the UI thread by polling, as on Vulkan.

### 3.2 Hazards and barriers

- V1 uses resources with Metal's default hazard tracking (`MTLHazardTrackingModeTracked`), created
  directly from the device, not from heaps. Metal then orders dependent encoders on the queue, which
  matches "the backend owns transitions".
- `MTLHeap` aliasing and `MTLHazardTrackingModeUntracked` are out of scope until the resource cache
  asks for them (`resources.md`). They need explicit `MTLFence` or `memoryBarrier` use and a design
  note first; adding them is a decision, not a refactor.
- A pass that samples a texture it renders to is rejected by the null backend and by
  `frames.md` section 4 on every backend; Metal does not get an exception.

### 3.3 Render-pass break and copy

A render pass maps to one render encoder; ending it to copy or sample the target ends the encoder.
Load and store actions come from the pass description. On Apple GPUs (tile-based) a forced break
stores and reloads attachments, so passes that break twice per frame cost real bandwidth. The glass
effect ([`../../specs/glass-v1.md`](../../specs/glass-v1.md) section 4, #242) needs
`copy_target_region` plus a pass break; on Metal that is a blit encoder between two render encoders.
The renderer decides break count; the backend must not add breaks of its own.

## 4. Resource lifetime

- The RHI retire queue is the authority. A resource is destroyed or reused only after the completion
  value of its last submission is reached. The backend does not rely on Metal's own retention for
  correctness.
- Command buffers are created with retained references (the default `commandBuffer`), not
  `commandBufferWithUnretainedReferences`. Dropping a list that is still executing is therefore safe
  (`rhi.md` section 5): Metal keeps what the encoded commands use. This is a safety net under the
  retire queue, not a replacement for it.
- Objective-C objects are held by the backend with ARC inside `src/rendering/backend/metal/`. No
  `id<...>` or `NS*` type appears in any header outside that directory, and Objective-C++ does not
  leak into the RHI headers.
- Retire entries are pushed from the UI thread and drained there. The completed handler may only
  publish the new completed value (an atomic store); it does not free resources. This keeps
  destruction on one thread and the retire queue free of locks in the steady state.
- Generations: resize and hot reload follow `rhi.md` section 6. A new swapchain generation retires
  the old drawables' textures after the last frame that referenced them completes.

### 4.1 Memory and storage modes

| Use | Apple silicon | Intel Mac (discrete or integrated) |
|---|---|---|
| Staging ring, uniforms | `Shared` | `Shared` |
| Textures, render targets | `Private` | `Private` |
| Readback | blit into a `Shared` buffer | blit into a `Shared` buffer; `Managed` needs `synchronizeResource:` |

Prefer `Shared` for CPU-written buffers and `Private` for GPU-only textures. Avoid `Managed`
(it needs explicit `didModifyRange:` and `synchronizeResource:` and exists only for discrete GPUs).
Staging and pending uploads use bounded queues, as in `frames.md` section 3, so a stalled GPU does
not grow CPU memory.

## 5. Presentation and `CAMetalLayer`

- Acquire the drawable as late as possible: after offscreen and glass passes are recorded, just
  before the pass that targets the window. A late acquire shortens how long a drawable is held.
- `nextDrawable` can block when all drawables are in use. Set `allowsNextDrawableTimeout` and treat
  a `nil` drawable as a skipped frame, the same result as `SwapchainError::Timeout` on Vulkan
  (`slots_skipped` increments, the UI thread does not wait).
- Set `maximumDrawableCount` to 2 or 3 to match `gpuFramesInFlight` (1..3). **Verify** that a
  frame slot and a drawable cannot deadlock when the two counts differ.
- Present with `presentDrawable:` encoded on the same command buffer before commit.
  `displaySyncEnabled` follows the pacing policy in [`../game-loop.md`](../game-loop.md).
- `drawableSize` is the pixel size from `WindowMetrics`, set on resize
  ([ADR-020](../../../decisions/ADR-020-window-resize-and-lifecycle.md)). Layer property changes
  happen on the main thread; `nextDrawable` may be called from the thread that records.
- The translucent window for glass needs a non-opaque layer and a pre-multiplied alpha format; see
  [`../../specs/glass-v1.md`](../../specs/glass-v1.md) section 6.

## 6. Errors and device loss

| Metal signal | RHI category |
|---|---|
| `MTLCommandBufferErrorOutOfMemory` | `out-of-memory` |
| `MTLCommandBufferErrorDeviceRemoved` or `MTLDeviceWasRemovedNotification` | `device-lost` (sticky) |
| validation layer message (`MTL_DEBUG_LAYER=1`) | `validation` (fatal in debug and tests) |
| other `MTLCommandBuffer.error` | `backend` |

An errored command buffer still signals the shared event so waiters do not hang: the completed
handler publishes the value and records the error, and the runtime applies the sticky device-loss
rule from `rhi.md` section 7. **Verify** that an errored buffer reliably runs its completed handler.

## 7. Shutdown

Stop new submissions, wait with a bound (`waitUntilSignaledValue:timeoutMS:`) for the last submitted
value, drain the retire queue, release drawables and the layer reference, then release the device.
No unbounded wait and no `waitUntilCompleted` on the UI thread.

## 8. Tests

- **Null contract tests** run against the Metal device on the macOS lane (#204 acceptance), and the
  same suite runs on null on every platform.
- **Retire queue** tests use a fake completion source, never sleeps (`rhi.md` section 9). The retire
  logic is currently inside the Vulkan backend (`src/rendering/backend/vulkan/src/`); #204 must
  either share it behind a small completion-source interface or reimplement it with the same tests.
  Prefer sharing, so the rule has one implementation.
- **Metal-only**: a timeline test like `vulkan_timeline_frames` (monotonic value, never ahead of a
  submit, at most N in flight, zero validation messages), a dropped-while-executing list, a
  `nil`-drawable skip, and a drawable resize.
- Conformance with `tolerance.json` (#161, #207) gates Tier 2.

## 9. Decisions and open points

Decided in [ADR-053](../../../decisions/ADR-053-native-metal-backend.md) (2026-11-13):

- Floor macOS 13 and iOS 16 (provisional; confirm the bounded-wait API on that floor when #204
  starts). `CAMetalDisplayLink` where present, `CVDisplayLink` otherwise.
- The retire logic is shared behind a completion-source interface; the extraction is the first
  commit of #204 and must leave the Vulkan retire tests unchanged.
- No heaps and no untracked resources until measurements justify them.

Still open:

- A second command queue for transfers (deferred with compute and transfer queues in `rhi.md`
  section 10).
- The checks marked **verify** above, which need a Mac.
