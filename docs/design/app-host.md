# App host: the production frame loop

Status: steps 3-8 are implemented by `tessera_app` (`include/tessera/app/host.hpp`,
`src/app/host.cpp`) with the headless contract in `tests/unit/app_host_tests.cpp`. The driver
(steps 1, 2 and 9: wake/pacing, OS input, present) is a window runner or a headless test; the host
never touches a window, swapchain or GPU device. Written 2026-11-10 after the
transform/z-index/box-shadow work, which left several finished steps with no caller; implemented
2026-11-11.

## 1. Problem

These parts exist and are tested in isolation; before the host nothing ran them together in the
documented order:

| Piece | Where | Called from |
|---|---|---|
| `FramePipeline::tick` / `finish_frame` / `paint_frame` | `src/ui/frame/` | `AppHost::begin_frame` / `paint` |
| `FramePipeline::sync_style` | same | `AppHost::begin_frame`, after layout |
| `SceneStyleTree` / `SceneStyleProvider` | `src/ui/style/`, `src/ui/paint/` | created by the host from its compiled stylesheet; rebuilt when style or layout work is needed |
| `apply_layout` and the layout engine | `src/ui/layout/` | `AppHost` layout pass |
| `AppHost::end_frame()` (dirty clear) and the texture caches' own `end_frame()` | `src/app/host.cpp`, `src/rendering/{path,image}/` | the host clears dirt; texture caches stay with the composition root that owns them (section 6) |
| `RuntimeLoop`, `FramePacer` | `src/runtime/` | the driver (window runner / test), passed into the host by reference |

The only other real loop, `tools/poc/batched2d/main.cpp`, pushes rectangles directly and uses none
of the style, layout or paint pipeline. The host is the component that owns this sequence.

## 2. Ownership

- The host owns: the scene root, `FramePipeline`, `DirtyPass`, `DamageTracker`,
  `DamageAccumulator`, the style tree and compiled stylesheet, the layout engine and the animation
  runtime.
- The host does not own the window, swapchain or GPU device. UI code sees `WindowMetrics` only
  ([ADR-020](../../decisions/ADR-020-window-resize-and-lifecycle.md)); the host receives metrics
  and a `PaintSink` plus a present callback from the platform layer.
- Everything runs on the UI thread. The frame loop never blocks on the GPU: no fence waits, no
  `wait_idle()`, no blocking I/O; async work completes through the `RuntimeLoop` completion queue.
  Teardown (the window driver returning or being destroyed) drains only the command lists it
  submitted, polling `is_complete()` under a bounded total budget, never a device-wide wait.

## 3. Frame order

One iteration, in this order (steps 3 to 5 are skipped when nothing is dirty):

1. **Wake and decide.** `RuntimeLoop::tick`; if it does not say render, park on the bounded
   platform wait until the pacer's next deadline (clamped by `app/idle_policy.hpp`). A static UI
   presents nothing.
2. **Input.** Drain the normalized input queue and dispatch to the scene; handlers mark nodes dirty.
3. **Tick.** `FramePipeline::tick(root, dt)` advances animations and collects dirty nodes. Its
   `needs_style` / `needs_layout` flags gate the next two steps.
4. **Style and layout.** Resolve computed styles for style-dirty nodes, run layout for
   layout-dirty subtrees, apply boxes with `apply_layout`.
5. **Sync.** `FramePipeline::sync_style` copies `z-index`, `transform` and `box-shadow` reach onto
   nodes. It must run after layout because transforms resolve against the laid-out border box, and
   before step 6 because it raises the paint/render dirtiness that damage consumes.
6. **Damage.** `finish_frame` collects Paint|Render dirt, derives damage (old and new bounds,
   shadow outsets, world scale) and the redraw plan. If `should_render` is false, clear dirt and
   return to step 1.
7. **Paint.** `paint_frame` with the plan: a partial plan wraps the sink in `ClippedPaintSink`
   (which forwards matrices, so rotated nodes stay exact), a full redraw paints directly.
8. **End of frame.** `AppHost::end_frame()` clears every dirty domain on the scene; a skipped frame
   calls it too, so dirt cannot stay latched into a phantom render request. Texture caches
   (`PathTextureCache`, `ImageTextureCache`) have their own `end_frame()` for textures retired this
   frame; they belong to the composition root that installs them in the sink, not to `AppHost`
   (section 6).
9. **Present.** Hand the finished frame to the platform present callback only when the host's
   damage/redraw plan has regions, or when the caller forces frames (bounded CI runs).

## 4. Decisions taken by the implementation

1. **Double style resolution stays.** Layout builds its input from `SceneStyleProvider::compute`
   and the paint pass resolves the same styles again through the provider. Measured cost is small
   at the current scene sizes; a "resolved style" cache waits for a profile.
2. **Dirty clearing lives in the host** (`AppHost::end_frame`), so a skipped frame still clears and
   a paint-only frame cannot leave stale dirt behind.
3. **Resize and DPI recreate the target.** The damage accumulator is constructed with its extent
   and the pipeline borrows it, so `resize` builds both again, clears the `DamageTracker` (old
   bounds are meaningless at the new size) and marks the whole subtree Layout|Paint|Render.
4. **The selector tree is rebuilt per geometry frame**, because attaching or removing a child
   changes the tree's shape and generated code attaches conditional branches. A paint-only frame
   skips the rebuild; idle frames do no work.
5. **The window root is host geometry.** The host writes `width`/`height`/`display:flex`/
   `flex-direction:column` as inline style on the root, which is the outermost cascade origin: an
   app stylesheet can style the root's background but cannot resize the window.
6. **Error policy is unchanged.** A throwing sink can still unbalance the transform/clip stacks;
   `paint_scene` does not use RAII guards yet (tracked in section 6).
7. **Headless mode.** `ManualClock` + `RuntimeLoop` + a recording or CPU sink drives exactly the
   same host calls; `tests/unit/app_host_tests.cpp` asserts first frame, idle, paint-only damage,
   resize and settling. The CPU sink paints transformed nodes as their AABB
   (`supports_transform()` is false), so rotation fidelity needs a matrix sink.

## 5. Not covered

Multi-window hosts (one host per window is the expected shape), power policy, and the
accessibility snapshot publication, which each have their own documents.

## 6. Implementation notes

- `AppHost` owns: compiled stylesheet, selector tree + provider, `LayoutEngine`, `DirtyPass`,
  `DamageTracker`, `DamageAccumulator`, `AnimationRuntime` and `FramePipeline`. It borrows the
  scene root and the `RuntimeLoop`; it owns no window, swapchain, device or paint sink.
- `paint()` takes the plan from the last `begin_frame()`. A forced paint with an empty plan (first
  exposure, post-recovery) paints the whole frame instead of returning nothing.
- `FramePipelineConfig` (DPI scale, damage margin, redraw policy, interpolation) is `HostConfig`'s
  `pipeline` field; `text_measurer` injects a real layout measurer (null keeps the deterministic
  stub used by the goldens).
- `samples/click-demo` is the first driver: the generated `Demo` component plus a compiled
  stylesheet, a visible Win32/X11 window with a Vulkan swapchain (two frames in flight, bounded
  wait on the reused slot), `InputPump` → `InputRouter` → generated handler → update scheduler
  flush → relayout → repaint, and a windowless `--capture` mode (offscreen target + readback) for CI
  and visual review. The runner is the `tessera_app_window` library: `WindowHost` owns the window,
  device, swapchain, renderer, text stack and the host loop, and `capture_frame` renders one
  offscreen frame through the same path, so tools and tests share it with the window. Swapchain
  images are not preserved, so a presented frame is a full redraw; the damage plan decides whether
  the frame happens at all.
- Texture caches still belong to the composition root: a sink built with `VectorPaintConfig` /
  `ImagePaintConfig` borrows them, and that owner calls their `end_frame()` after the frame's
  commands finish. `AppHost::end_frame()` only clears scene dirty flags, and `WindowHost` installs
  no vector/image caches, so it has no texture-cache `end_frame()` to call. Promoting cache
  ownership into the host needs a painter abstraction (next slice).
- `WindowHost` is the concrete driver of steps 1, 2 and 9 (`app/idle_policy.hpp`). It idles on the
  `FramePacer` deadline (`pacer_wait_seconds` / `idle_wait_seconds`, clamped to
  `kMaxIdleWaitSeconds = 250 ms`) instead of sleeping a fixed amount and never sleeps past the
  deadline; the settled path parks before `begin_frame`, so a static scene does no style, layout or
  paint work. A frame the host did not make due calls `end_frame()` and paints nothing. Only a
  frame whose damage/redraw plan has regions is presented (the forced `run(max_frames > 0)` CI path
  is the exception). `run()` can be called again: the input queue is reattached on entry, and
  teardown polls only the submitted command lists under a bounded budget instead of a device-wide
  `wait_idle()`.
- `WindowHost::post_input` is the automation/test seam: it queues normalized events exactly as the
  platform translator does, so a window can be driven without a mouse/keyboard and the same pump,
  router, focus and generated handlers run. `tests/gpu/app_window_input_tests.cpp` uses it for
  click -> generated handler -> state and pointer focus + typed text -> generated onChange on a real
  hidden window + swapchain.
- Resize/DPI coverage: `AppHost::resize` (target recreation, subtree marks, layout reflow) is covered
  by `app_host`; `WindowHost::apply_metrics` feeds a `WindowMetrics` snapshot through the same path
  as an OS resize/DpiChange event (swapchain + host resize + frame request) and
  `tests/gpu/app_window_input_tests.cpp` asserts the 480x320 @ 2x reflow and 960 px damage target
  over a real hidden window; `capture_frame` covers the scaled offscreen target (640x400 at scale 2
  -> 1280x800). The OS-driven resize remains covered by the HAL window tests and the windowed POC.
- Known limits: `UpdateScheduler` is installed by the driver (`AppHost::set_update_scheduler`), not
  created by the host; the style tree rebuild is O(n) per geometry frame; resize reuses the previous
  `LayoutEngine` text cache; `:hover`/`:active` work through `InputPump` → `InputRouter` (the
  router sets the pseudo-state and the next begin_frame re-resolves style), and the hovered node's
  resolved `cursor` is mapped onto the HAL's `CursorShape` right after input each frame (only on a
  change, `src/app/window_cursor.*`; window-chrome-v1.md section 8.5); generated controlled `TextInput`s now
  publish caret/selection/preedit decoration (`TextInputRuntime::publish_decoration`, re-published at
  the end of `apply_state` because it rebuilds `PaintData`), but caret blinking keeps the paint
  default phase and a caret appears only once the field handles an event (Focus/Blur are not bound:
  the node binding budget is 8 slots and the text-input wiring uses 7).

Related: [dirty-rendering.md](dirty-rendering.md), [game-loop.md](game-loop.md),
[gpu/frames.md](gpu/frames.md), [animation-runtime.md](animation-runtime.md),
[../specs/stylesheet-v1.md](../specs/stylesheet-v1.md).
