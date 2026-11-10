# Dirty rendering and invalidation

Status: design for Big Step B2 (runtime slice) and Big Step E (data-heavy UI); both are defined in
[AGENTS.md Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step). The node dirty-domain
bitmask, ancestor propagation, and pruned collect/clear pass are implemented in
`include/tessera/ui/dirty.hpp`, `src/ui/scene/node.cpp`, and `src/ui/scene/dirty_pass.cpp`;
damage accumulation (clip/merge/cap/full-redraw fallback) is implemented in
`include/tessera/rendering/damage.hpp` and `src/rendering/damage/damage.cpp`; damage-to-scissor
planning and the offscreen scissor replay (`begin_damaged`) are implemented in
`include/tessera/rendering/redraw_plan.hpp`, `src/rendering/damage/redraw_plan.cpp`, and
`src/rendering/gpu/batched2d_renderer.cpp`; the per-frame orchestrator that turns animation samples
into target dirty flags, then dirty nodes into damage and a redraw plan, is
`include/tessera/ui/frame_pipeline.hpp` (`tessera_frame`). Two-frame offscreen equivalence is
covered by `damage_equivalence_tests`: a full frame followed by a `LoadOp::Load` partial frame is
bit-identical to a fresh full frame, both for static content and for a moved/recolored card whose
damage region covers old and new bounds; an undersized damage region is asserted to leave stale
pixels, so the equality check is not vacuous. The Vulkan command list preserves the target layout
across load frames. Retained caches/layers remain
design-only. Decision record:
[ADR-018](../../decisions/ADR-018-dirty-rendering-and-frame-pacing.md) (dirty rendering and frame
pacing). Companion to [`game-loop.md`](game-loop.md) (runtime loop and pacing) and
[`renderer.md`](renderer.md). Terms such as golden, damage, batch key, Yoga and `ComputedStyle` are in
[`glossary.md`](../reference/glossary.md); a golden is a deterministic reference image from the
software path ([ADR-009](../../decisions/ADR-009-deterministic-headless-golden-tests.md)).

## 1. Goals and invariants

- Do work proportional to what changed, not to tree size.
- Never show a stale frame when a visible input/style/layout change occurred.
- Dirty and full redraw must produce identical pixels in golden tests.
- Caches are bounded and evictable; no unbounded growth under animation.
- No CPU-GPU readback in the hot path.
- Invalidation order is deterministic: style -> layout -> paint -> render.

Correctness beats skipping a frame. If the runtime cannot prove a partial update is safe, it
falls back to a full redraw (repaint the whole target).

Virtualized lists/grids reduce the number of realized nodes; they do not replace damage/culling.
See [`virtualization-v1.md`](../specs/virtualization-v1.md) for the collection model.

## 2. Dirty domains

| Domain | Set by | Consumed by | Cleared after |
|---|---|---|---|
| `StyleDirty` | class/id/attribute/pseudo-state/theme/inline-style change | style resolver | computed style updated |
| `LayoutDirty` | layout-affecting style change, text/font change, child add/remove, resize | Yoga/layout pass | bounds updated |
| `PaintDirty` | paint-affecting style change, transform/clip/visibility, layout move/resize, texture/atlas change | paint/draw-list builder | draw list updated |
| `RenderDirty` | any paint change, target resize, damage, explicit request | renderer | frame presented |
| `ResourceDirty` | texture/font/atlas/GPU buffer content change | resource uploader (CPU-to-GPU copy step) | upload recorded |
| `GpuDirty` | buffer/image contents changed on CPU | command recording | commands submitted |

Each node stores a bitset. A node may be dirty in several domains at once. The zero value is
`Dirty::Clean` (not `None`): bitmask enumerators must avoid X11 macro names such as `None`,
`Status`, `Bool`, `Success`, `True`, `False` and `Expose`, because a translation unit that includes
Xlib would rewrite them before the compiler sees the declaration.

### 2.1 Granularity

- **Node**: default unit for style/layout/paint.
- **Subtree**: used when a parent change invalidates descendants (visibility, `display:none`,
  clip, transform, theme scope).
- **Layer**: a retained cache unit for a stable subtree (card, chart, panel).
- **Target**: render target/swapchain image dirty flag.

### 2.2 Propagation

When a node is marked dirty:

1. Set the flag on the node.
2. Walk ancestors and set an `has_dirty_descendant` aggregate bit.
3. Stop early if the aggregate bit is already set.
4. Do not walk descendants unless the domain requires it (for example a subtree invalidation).

This keeps marking O(depth) in the common case.

## 3. Change classification

| Change | Style | Layout | Paint | Render |
|---|---|---|---|---|
| class/id/attribute | yes | if layout property matched | if paint property matched | if visible/geometry changed |
| `:hover`/`:active`/`:focus` | yes | only if the matched rule changes layout | yes if the matched rule changes a paint property | yes |
| inline style | yes | if layout property | if paint property | yes |
| theme/custom property | affected nodes | if layout property | if paint property | yes |
| width/height/margin/padding | recompute | yes | yes | yes |
| color/background/border-radius | recompute | no | yes | yes |
| transform/opacity | recompute | no | yes | yes |
| visibility/display | recompute | yes | yes | yes |
| text/font change | recompute | yes | yes | yes |
| child add/remove | new/changed nodes | yes | yes | yes |
| texture/atlas content | no | no | yes (textured nodes) | yes |
| async resource ready | no | no | yes (nodes using it) | yes |
| viewport resize | root | yes | yes | yes + target recreate |

A style resolver can cheaply compare the new computed layout/paint groups with the previous
values and only raise the domains that actually changed.

## 4. Pipeline

### 4.1 Style

- Resolve only `StyleDirty` nodes and their affected pseudo-states.
- Selector matching uses the indexed rule buckets (id/class/tag).
- Cache `ComputedStyle` per `(node, pseudo-state, theme-scope)`.
- If the layout group changed, mark `LayoutDirty`; if the paint group changed, mark `PaintDirty`.
- Clear `StyleDirty` after the new computed style is stored.

### 4.2 Layout

- Mark Yoga nodes dirty when layout-affecting properties change.
- Run layout only for dirty roots/subtrees; compute new bounds.
- Compare old/new world bounds:
  - moved/resized -> mark paint (+ descendants if clipping/overlap).
  - size changed -> mark text measurement and glyph layout.
- Clear `LayoutDirty` after bounds are updated.
- Layout may run at tick rate; paint can run at render rate.

### 4.3 Paint

- Rebuild draw lists only for `PaintDirty` nodes/layers.
- A retained node with no dirty descendants reuses its cached draw list.
- A dynamic node (chart/animation) streams vertices into the current frame arena.
- Batch key `(scissor, texture)` ([glossary](../reference/glossary.md), [`renderer.md`](renderer.md)) is unchanged from the B1 renderer; dirty rebuilds must produce the
  same ordering as a full rebuild.
- Clear `PaintDirty` after the draw list/layer is rebuilt.
- Mark `RenderDirty`.

### 4.4 Render

- Render if `RenderDirty`, damage is non-empty, an animation is active, or continuous mode is on.
- Render targets:
  - **Full redraw**: clear + repaint entire target. Safe default.
  - **Damage redraw**: clear/redraw only damage rectangles (see §6).
- Present; clear per-frame render flags.
- Never read back the target to decide dirty state.

`FramePipeline` (`ui/frame_pipeline.hpp`) implements this split for one frame:

1. `tick(root, dt)` advances animations through the loop bridge, marks each sampled target with the
   dirty domains of its declarations, and collects the dirty pass. It reports `needs_style` and
   `needs_layout` so the caller can run those stages.
2. The caller resolves style and layout; `apply_layout` and node updates mark paint/render dirty.
3. `finish_frame(root)` re-collects paint/render dirty, updates the `DamageTracker`, and returns the
   normalized `RedrawPlan` (`should_render`, `full_redraw`). An empty plan means no frame.

The pipeline owns the damage accumulator for the frame: `tick` clears it, so extra damage must be
added after `tick` and before `finish_frame`.

## 5. Retained/immediate hybrid

### 5.1 Retained draw lists

- A stable subtree can be cached as an ordered draw list (vertices/indices + batch metadata).
- Cache key: node id + style revision + layout revision + theme revision + clip/transform version.
- Reuse when the key and all descendant revisions are unchanged.
- Evict under memory pressure; prefer layers with low reuse.

### 5.2 Layer promotion

Promote a subtree to a cached layer when:

- it is expensive to repaint (large chart, complex card), and
- it changes rarely, or
- it needs independent opacity/transform/clip isolation.

Layer promotion is a heuristic, not a correctness requirement. A layer can always be repainted.

### 5.3 Immediate streams

Charts, live data, and animations stream vertices into a per-frame arena:

- ring buffer per series; only rewrite changed ranges;
- vertex buffer orphaning or per-frame slices;
- no full-tree rebuild;
- bounded memory: capacity is part of the chart pipeline contract.

## 6. Damage and partial redraw

### 6.1 Damage rectangles

- Accumulate axis-aligned damage rects from old/new bounds of changed nodes.
- Union/merge nearby rects; cap the count (16 is an example; the real cap is not specified); exceeding the cap -> full redraw.
- Clip damage to the target.
- Expand by the blur/shadow/AA margin of the changed nodes. Implemented: `Node::paint_outsets`
  (set from `box-shadow` by `sync_scene_style`) widens a node's damage by the shadow reach, scaled
  by the node's world scale; transformed nodes damage their world AABB.

### 6.2 When partial redraw is safe

Safe when:

- changed nodes do not overlap translucent layers whose composition would change;
- no backdrop filter/blur affecting the region;
- no shadow/glow crossing the damage boundary;
- the swapchain/present path supports damage (or the target is offscreen).

Otherwise fall back to full redraw. This is conservative by design.

### 6.3 Present-path damage

- **Vulkan**: optional `VK_KHR_incremental_present`; otherwise swapchain full-image present.
- **Windows/DXGI**: dirty rects with flip-model swapchains when the OS/driver supports them.
- **Wayland**: surface damage; the compositor decides.
- **X11**: Present extension damage.
- Offscreen/headless: always full target for deterministic goldens.

The B1 offscreen renderer always full-redraws; B2 adds the offscreen scissor-based damage replay
described in §6.4. Swapchain/OS present-path damage stays B3 (window/swapchain phase of Big
Step B).

### 6.4 Offscreen damage mechanics (B2)

`gpu::Batched2DRenderer` exposes `begin_damaged(commands, target, desc, plan)`. `begin()` remains
the full-redraw entry point and its command stream is unchanged.

- **Full plan:** `plan.full_redraw == true` delegates to the normal `begin()` path, so full-redraw
  output is byte-identical to the pre-damage renderer.
- **Partial plan:** each flushed batch is replayed once per damage region with
  `rhi::intersect(batch.scissor, region)` applied before the draw. Every emitted draw therefore
  lies inside exactly one plan region. Regions must be disjoint; `DamageAccumulator` guarantees
  this by merging touching rectangles, which also prevents translucent geometry from being blended
  twice. A batch that intersects no region emits no draw.
- **Empty plan:** a non-full plan with no regions is a true no-op: no render pass is opened and no
  draw is recorded, so the caller should skip submission for that frame.
- **Load op:** a partial redraw begins the target with `LoadOp::Load`; the renderer never clears
  undamaged pixels itself. The `batched2d` POC keeps `Clear` because it renders one synthetic
  frame.
- **Fallback:** `should_full_redraw()` still owns the area/scissor-count policy; when it returns
  true the caller sets `plan.full_redraw` and the renderer takes the full path. Damage that is not
  provably safe (overlapping translucent layers, backdrop blur, shadows crossing the boundary)
  follows the same fallback.
- **Determinism:** the null backend records the active scissor for every draw
  (`NullCommandStats::draw_records`), so containment is asserted without a GPU.

Swapchain dirty rects and incremental present remain B3.

### 6.5 Accumulator clear and full-redraw latch

`DamageAccumulator` has exactly one piece of state that can force a full redraw without being
derived from the current rectangles: `full_redraw_`, the fallback latch set when the configured
rect cap (`DamageConfig::max_rects`) is exceeded. `clear()` resets both the damage rectangles and
that latch; configuration (`target`, `max_rects`) and allocated capacity persist, so a cleared
accumulator is semantically identical to a freshly constructed one with the same configuration.

The other full-redraw decision is not state at all: `should_full_redraw()` recomputes the area and
scissor-count policy from the plan's current regions on every call, and the caller (frame pipeline)
copies the policy result into a `RedrawPlan` value. Nothing in the plan or the policy survives a
`clear()`.

Invariant (regression-tested by `tests/unit/damage_clear_tests.cpp`): after `clear()`, an empty
accumulation yields an empty plan (no render pass, no presents) and a small accumulation yields a
partial plan at exactly the same thresholds as a fresh accumulator; a whole-screen change that made
the previous plan full through the policy - or a cap-latched full redraw - cannot leak into the next
frame. This is what makes the power contract ("static UI produces zero presents/submissions after
settling") hold for the damaged path. `DirtyPass::overflowed_` is per-collection state and resets
on every `collect()`; the only intentional cross-frame latch is the frame lane's
`FramePipeline::frame_due_`, which `tick()` resets.

### 6.6 Dirty-pass overflow forces a full redraw

`DirtyPass` has a bounded node cap (`DirtyPass(max_nodes)`, default **1024**; the vector is reserved
to the cap, so collection never grows past it). When more nodes are dirty than the cap, `collect()`
stops adding nodes and sets `overflowed()`; the returned list is then a **truncated view** of the
changed set.

Under-damage is unsafe: a missing node may have changed any pixel inside its (unknown-to-the-damage
pass) bounds, so a partial plan can leave **stale pixels** on screen indefinitely. Over-damage only
costs one full repaint and is always correct, so the rule is:

- On `DirtyPass::overflowed()`, `add_scene_damage` and `DamageTracker::add_scene_damage` call
  `DamageAccumulator::force_full_redraw()`: the partial rectangles are dropped and the accumulator's
  full-redraw latch is set, so `make_redraw_plan()` returns the whole-target region and
  `DamagePassResult::full_redraw` reports the frame as full.
- `FramePipeline::finish_frame()` also forces the plan to full when `FrameRender::damage.overflowed`
  (defense in depth for a future plan source; the plan decision is unchanged otherwise).
- The latch is accumulator state, so `DamageAccumulator::clear()` releases it: the next frame whose
  dirty count fits under the cap is partial again, and an idle frame after settling still produces
  an empty plan (no render pass, no presents), preserving the power contract.
- Cost: one full redraw for the overflowing frame only. The alternative - a
  bounded-but-complete pass that never truncates - was rejected: the changed set is unbounded in
  the worst case and would break the memory bound the cap exists to provide.

Tests: `damage_pass` (overflow result/latch/plan and the cap boundary: exactly cap stays partial,
cap + 1 is full, both damage-pass and tracker paths), `damage_tracker` (same rule through the
tracker), `frame_pipeline_overflow` (overflow frame paints byte-identically to a full redraw, the next
under-cap frame is partial, the settled frame is idle, determinism), and `damage_clear` (the latch
clears).

## 7. GPU resources and synchronization

- `gpuFramesInFlight` frame slots; each slot owns vertex/index/uniform arenas.
- Never write a slot still in flight; timeline semaphores gate reuse.
- Prefer persistent mapped buffers for dynamic streams; use orphaning/per-frame slices.
- Texture/atlas uploads are batched; dirty atlas regions upload once.
- Descriptor sets are cached per texture/sampler; no per-draw allocation.
- No `vkDeviceWaitIdle`/readback in the frame loop except for teardown/debug.
- Resource lifetime: the renderer records what a frame uses; release waits for the frame slot.

## 8. Animations, charts, and data streams

- Fixed-step animation writes to the simulation state; the renderer interpolates visual values.
- Only nodes with active animations are `PaintDirty` each tick.
- Chart series use ring buffers; update only the changed index range.
- LOD/decimation can mark only the changed vertex range, not the whole series.
- Text/glyph atlas changes mark only nodes using the affected glyphs.
- Continuous streaming must not rebuild the scene graph or draw lists wholesale.
- Async resource completions apply on the UI thread and mark only the nodes that use the
  resource; see [`async.md`](async.md).

## 8.1 Power and utilization

Dirty rendering is the primary mechanism that keeps an application framework from behaving like
a full-load game loop:

- a clean/static subtree produces no draw work and no GPU submissions;
- occlusion/minimization pauses style/layout/paint/render work;
- only dirty nodes and damage rectangles are processed;
- retained layers/draw lists avoid repeated CPU work;
- adaptive quality lowers optional cost (render scale/effects) when budgets are missed;
- `powerPolicy=adaptive` is the default; `benchmark` (uncapped/full redraw) is diagnostic only.

A dirty frame is not a license to repaint the world. If damage is unsafe or above the damage-area threshold (`fullRedrawThreshold`, default 0.6 in
[`runtime-config-v1.md`](../specs/runtime-config-v1.md)),
fall back to full redraw, measure it, and report why. See [`performance.md`](performance.md).

## 9. Metrics and debugging

Counters:

- `dirty_style_nodes`, `dirty_layout_nodes`, `dirty_paint_nodes`
- `rebuilt_draw_lists`, `reused_draw_lists`, `layer_cache_hits/misses`
- `damage_rects`, `damage_area_ratio`, `full_redraw_reason`
- `draw_calls`, `vertices`, `bytes_uploaded`
- `frame_time_ms`, `paint_time_ms`, `render_time_ms`

Debug overlay: dirty counts, draw calls, damage ratio, cache hit rate, full-redraw reason.
Tests assert both visual output and work bounds.

## 10. Testing

- **Equivalence**: dirty-only render vs full redraw must be pixel-identical in golden mode.
- **Invalidation matrix**: every change class in §3 produces the expected dirty domains.
- **Cache correctness**: class/id/state/theme/transform/clip changes invalidate the right caches.
- **Damage**: damage rects cover all changed pixels; conservative fallback is exercised; the null
  backend proves every partial draw is scissored inside a plan region.
- **Stress**: 10k nodes, 1M chart points, rapid resize/theme switching; no unbounded memory.
- **Leak checks**: layer/draw-list caches are bounded and evictable.

## 11. Anti-patterns

- Marking the root `PaintDirty` every frame.
- Repainting a whole dashboard for a single sparkline update.
- Reading the framebuffer to decide what changed.
- Unbounded layer/draw-list caches.
- Recreating pipelines/descriptor sets per frame.
- Ignoring shadow/blur margins in damage rects.
- Using wall-clock timestamps inside dirty logic.

## 12. Open questions

- Damage-rect support across all target platforms and compositors.
- When to promote/demote layers automatically.
- Multi-threaded paint/draw-list building.
- Occlusion/culling integration with the scene graph ([`scene-graph.md`](scene-graph.md)).
- Exact cache budget defaults for desktop vs embedded (see [`buffer-config-v1.md`](../specs/buffer-config-v1.md) for the buffer budget schema).

## 8. Render-side damage in the present loop (D4)

Damage is **render-work reduction**, not partial OS presentation: the present path still presents the
full surface, but a due frame with a small damage plan is recorded through
`Batched2DRenderer::begin_damaged` (scissor regions) into a preserved render target
(`LoadOp::Load`) instead of a full clear+redraw. A whole-screen/large plan falls back to full redraw
via the existing `RedrawPolicy` (`full_redraw_area_ratio`, `max_scissors`). The POC prints per-frame
redraw regions, covered area, draw calls and batches; the offscreen `damage_present` test renders a
scripted sequence cumulatively and compares it at tolerance 0 with a full redraw through the **same
renderer/sink** (a fresh renderer's glyph atlas packs differently, so filtered glyph edges can
differ — that is a reference artifact, not a damage bug).

Measured on the local AMD GPU (240x160 target, Debug, validation on): initial frame full; moving a
card -> 1 region / 4,608 px (12% of the target); changing a wrapped text run (with caret +
selection decoration) -> 1 region / 9,840 px (26%); toggling a background -> 1 region / 1,156 px (3%);
all three frames pixel-identical to the same-sink full redraw; a whole-screen change reports
`full_redraw` and matches; empty damage renders nothing. Draw calls for the partial frames are the
same 3 batches (every batch intersects the damage rect); the win is rasterized area, not draw count,
and this is documented rather than over-claimed.

Per-primitive damage table (conservative by design; do not tighten without evidence):

| Primitive | Damage rect |
|---|---|
| Solid/rounded rect, background/border | node border box; on a move, old **and** new bounds (DamageTracker keeps prior bounds) |
| Text run (wrap/no-wrap, max_lines) | text node content box (paint only emits inside it); per-line tightening is deferred |
| Caret/selection decoration | within the text content box (line boxes + 1-2 px caret); no extra margin needed in the tested cases |
| Vector/SVG asset | node content box used for raster placement |
| Clip/rounded clip | parent clip box (children damage from their own bounds; clip is not an extra damage source) |
| Opacity/transform | no extra damage; transform uses the node's world AABB |

Conservative/known gaps: text damages the whole text node rather than the changed lines; a glyph
whose ink overhangs its node box slightly should add a damage margin (the `unknown node` margin path
in `add_scene_damage` is the place). Dirty-pass overflow does **not** under-damage: a truncated
dirty list forces a full-target redraw (§6.6, task-65), so overflow frames are always safe. Do not
enable partial present/VRR from this: D4 keeps those deferred.

Tests: `damage_min` (solid node move, exact at margin 0 and 2) and `damage_present` (scripted
card/text/decoration/background changes, partial regions/area, whole-screen fallback, idle/no
render, two-run determinism). `DamageAccumulator::clear()` latched-state regressions are covered by
task-62; the present-loop test uses a fresh accumulator for the idle assertion.

### 8.1 Per-line text damage and ink margin (task-66)

Text nodes no longer automatically damage their whole content box. `ui::layout::node_damage_rect`
uses the node's `PaintData` text with the same deterministic stub metrics/line layout as the paint
fallback and returns a **vertical** union of the emitted line boxes plus `kTextDamageMargin = 4 px`;
the width stays the node width. The 4 px margin covers glyph ink overhang (~1 px), a 2 px caret at
device scale >= 2, and the damage layer's lack of computed padding/alignment knowledge. Old+new
union still comes from `DamageTracker` (it stores this rect on the previous frame), so a move or a
text change damages both old and new line unions.

Conservative fallbacks: non-Start `text_align`, any decoration payload, empty/zero-font text, and a
missing font-size override keep the full content box; real-metrics sources should likewise keep the
box until damage can share the paint metrics source. Measured in `damage_present` (240x160): the
text+decoration change dropped from 9,840 px to **6,720 px** of damage (26% -> 17.5% of target)
while remaining pixel-identical to the same-sink full redraw at tolerance 0; `damage_min` pins the
solid-node rule.

Draw-call finding: partial frames still report 3 draws/3 batches, identical to the full frame. Every
batch's scissor intersects the single damage region, and the existing damaged path already emits one
scissored draw per (region, intersecting batch). Splitting batches per region cannot reduce draw
calls here (it would multiply them by region count) and region-local vertex filtering would need
per-frame vertex storage; the measured win is rasterized area, so no renderer change was made.

### 8.2 POC damage probe (task-66)

`batched2d_poc --backend vulkan --on-demand --damage-probe --validation` prints, for every due frame,
`damage-probe: frame regions=N area=N pct=N full=0|1 draws=N batches=N`, and a final
`damage-probe: summary rendered=N presented=N idle_waits=N`. Idle frames (no scene change) print no
frame line and present nothing. Presentation stays full-frame into the swapchain (swapchain images
are not guaranteed to preserve prior contents and an acquired image is not in `PRESENT_SRC`); the
probe reports the redraw plan/work without performing unsafe partial present. Preserved-target
partial rendering is exercised by `damage_present` offscreen. Measured on the local AMD at 240x160:
initial frame full; two scripted scene changes each report 1 region / 17,280 px (20% of target);
3 presents with 3 idle waits; output is deterministic across runs and validation-clean.

### 8.2 Text damage tightening reverted (review follow-up)

The 8.1 line-union rule is withdrawn: `node_damage_rect` returns the node's world bounds for every
kind. Paint offsets glyph runs by the computed padding, wraps at the padded width and measures with
the context metrics source, none of which the damage layer sees, so with `padding-top: 20px` or a
real font the union could end above/inside the drawn glyph rows and leave stale pixels. Re-tighten
only once damage shares the paint pass's content box and metrics source.
