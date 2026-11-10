# App host: the production frame loop

Status: design only. Nothing here is implemented; the pieces it connects are. Written 2026-11-10
after the transform/z-index/box-shadow work, which left several finished steps with no caller.

## 1. Problem

These parts exist and are tested in isolation, but no production code runs them together:

| Piece | Where | Needs a caller to |
|---|---|---|
| `FramePipeline::tick` / `finish_frame` / `paint_frame` | `src/ui/frame/` | animate, derive damage, paint |
| `FramePipeline::sync_style` | same | apply z-index, transform, shadow reach |
| `SceneStyleTree` / `SceneStyleProvider` | `src/ui/style/`, `src/ui/paint/` | resolve computed style |
| `apply_layout` and the layout engine | `src/ui/layout/` | write bounds |
| texture caches' `end_frame()` | `src/rendering/{path,image}/` | free parked textures |
| `RuntimeLoop`, `FramePacer` | `src/runtime/` | decide when to render |

The only real loop, `tools/poc/batched2d/main.cpp`, pushes rectangles directly and uses none of
the style, layout or paint pipeline. The host is the component that owns this sequence.

## 2. Ownership

- The host owns: the scene root, `FramePipeline`, `DirtyPass`, `DamageTracker`,
  `DamageAccumulator`, the style tree and compiled stylesheet, the layout engine and the animation
  runtime.
- The host does not own the window, swapchain or GPU device. UI code sees `WindowMetrics` only
  ([ADR-020](../../decisions/ADR-020-window-resize-and-lifecycle.md)); the host receives metrics
  and a `PaintSink` plus a present callback from the platform layer.
- Everything runs on the UI thread and never blocks (no fence waits, no blocking I/O). Async work
  completes through the `RuntimeLoop` completion queue.

## 3. Frame order

One iteration, in this order (steps 3 to 5 are skipped when nothing is dirty):

1. **Wake and decide.** `RuntimeLoop::tick`; if it does not say render and no input is queued,
   park until the next wake. A static UI presents nothing.
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
8. **End of frame.** Call `end_frame()` on each texture cache so textures retired this frame are
   released (deferred destruction, bounded by the cache limits), then clear dirty flags.
9. **Present.** Hand the finished frame to the platform present callback.

## 4. Decisions this design needs from its implementer

These are real open points, not settled ones:

1. **Double style resolution.** Layout needs style, and `sync_style` calls `compute` again after
   layout. Whether `SceneStyleTree` caching makes the second pass nearly free is not measured;
   measure before adding a separate "resolved style" cache.
2. **Where dirty clearing lives.** `finish_frame` collects; something must clear. The PoC clears
   in its loop; the host should own it so a skipped frame still clears.
3. **Resize and DPI.** A metrics change should mark layout and full-redraw dirty and re-run steps
   4 to 9; the damage target size must be updated first.
4. **Error policy.** A thrown paint or sink error must not leave the transform/clip stacks
   unbalanced; `paint_scene` pushes and pops in matching order but does not use RAII guards.
5. **Headless mode.** The same host with `ManualClock` and the CPU sink is the deterministic
   end-to-end test harness. The CPU sink paints the bounding box of transformed nodes
   (`supports_transform()` is false); tests of true rotation need a sink that supports matrices.

## 5. Not covered

Multi-window hosts (one host per window is the expected shape), power policy, and the
accessibility snapshot publication, which each have their own documents.

Related: [dirty-rendering.md](dirty-rendering.md), [game-loop.md](game-loop.md),
[gpu/frames.md](gpu/frames.md), [animation-runtime.md](animation-runtime.md),
[../specs/stylesheet-v1.md](../specs/stylesheet-v1.md).
