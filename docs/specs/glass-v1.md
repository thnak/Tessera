# Glass v1: backdrop blur and translucent windows

Status: proposed, ships in V1 (Windows and Linux; owner decision of 2026-11-13). It adds one paint
capability the renderer does not have today, `backdrop-filter`, and one window capability, a
translucent window with an OS backdrop. Builds on [`stylesheet-v1.md`](stylesheet-v1.md),
[`window-chrome-v1.md`](window-chrome-v1.md), [`dirty-rendering.md`](../design/dirty-rendering.md),
[`paint-pass.md`](../design/paint-pass.md) and [`performance.md`](../design/performance.md).

Implementation status (2026-11-14, #244 complete): the style surface (section 2), the paint-pass
segmentation, the damage rules of section 7, the config keys, the two-pass shader sources and the
**GPU composite** are implemented and tested. `Batched2DRenderer::draw_backdrop` executes a glass
node inside the frame it belongs to - break the pass, copy the 3-sigma dependency region into a
pooled target, run the horizontal pass into a second pooled target, resume with `LoadOp::Load` and
composite the vertical pass through the rounded clip with the color matrix and the node opacity
folded in ([`gpu/offscreen-layers.md`](../design/gpu/offscreen-layers.md) section 5.1). The passes
read the sampled view of an offscreen target that RHI 1.3 (issue #254) provides
([`gpu/rhi.md`](../design/gpu/rhi.md) section 5.1), and `Batched2DSink::draw_backdrop` drives it from
the real paint path. Section 6 (`window-backdrop`) landed as #243. Section 8
records the `glass_ms` semantics, section 9 the tests, and section 12 the work items.

Terms: **Backdrop** is what was already painted behind a node in the same window. **Glass node** is a
node with a non-`none` `backdrop-filter`. **Blur target** is the offscreen texture the backdrop is
copied into and blurred. **Isolated layer** is a subtree painted into its own texture (a seam the renderer does not have yet; issue #227 and the layer-seam issue name it in
[`paint-pass.md`](../design/paint-pass.md); shared with shared-element snapshots and stacking
contexts). **OS backdrop** is the material the operating system draws behind a translucent window
(Mica, Acrylic, a compositor blur). See also the [glossary](../reference/glossary.md).

## 1. Goals and non-goals

Goals:

- A React/CSS-compatible way to frost what is behind a card, sheet, toolbar or popup:
  `backdrop-filter: blur(20px) saturate(1.4)`.
- A translucent window whose background is the OS material where one exists, with the same CSS
  surface for the in-window and the window-level effect.
- Deterministic output on lavapipe, so glass has goldens like every other paint feature.
- No cost when unused: a tree without a glass node pays nothing, and a static glass screen still
  idles with zero presents.

Non-goals (stay open after V1; named so they are not mistaken for omissions):

- Lens and refraction (Liquid Glass style bending of the backdrop). A later `backdrop-filter`
  function; the pipeline of section 5 does not preclude it.
- General `filter` on a node's own content (drop shadow, hue-rotate) and `mix-blend-mode`.
- Blur of content from a different window, a video surface or a native child window (the OS
  compositor owns those pixels).
- Animated blur radius at 60 fps on every device (allowed, see section 8, not guaranteed).

## 2. Style surface

| Property | Values | Notes |
|---|---|---|
| `backdrop-filter` | `none`, or a space-separated list of `blur(<length>)`, `saturate(<number>\|<percent>)`, `brightness(<number>\|<percent>)`, `contrast(<number>\|<percent>)` | Applied in written order, as in CSS. Any other function is `UnsupportedValue` and the whole declaration is ignored, like the other unsupported values. |
| `-webkit-backdrop-filter` | same | Alias, accepted for pasted web CSS. |
| `window-backdrop` | `none`, `auto`, `mica`, `acrylic`, `blur` | Only meaningful on the root `Window` element. `auto` picks the best material the platform has (section 6). |

- `blur(<length>)` takes a standard deviation in CSS pixels, clamped to `[0, glassMaxBlurRadius]`
  (default 64). Percentages and negative lengths are invalid. `blur(0)` is a pass-through.
- The glass node's own `background-color` is painted **over** the filtered backdrop, so
  `background: rgb(255 255 255 / 0.12)` gives the usual frosted tint. A glass node with an opaque
  background is legal and costs the blur for nothing; the engine does not detect it.
- The filter region is the node's border box clipped by its ancestors' overflow clips and by the
  window, shaped by its `border-radius` (the same rounded-rect SDF clip as the paint pass).
- `backdrop-filter` is not inherited and does not create a stacking context in V1 (see
  [`stylesheet-v1.md`](stylesheet-v1.md) section 11): the backdrop is everything painted earlier in
  the same parent's paint order, plus everything beneath, up to the window. A glass node therefore
  sees its parents' background and earlier siblings, never later siblings.
- Tokens: `--glass-blur`, `--glass-tint`, `--glass-saturate`, `--glass-border` are conventions of
  the bundled `Instrument` theme, not engine features ([ADR-032](../../decisions/ADR-032-bundled-component-library.md)).
  A theme builds `.glass { backdrop-filter: blur(var(--glass-blur)) saturate(var(--glass-saturate)); }`.
- `@supports (backdrop-filter: blur(1px))` is true wherever glass is implemented, so a stylesheet can
  carry its own fallback background.

## 3. Behavior

1. A glass node reads the backdrop that already exists in the current render target at the moment
   it is painted. Nothing painted after it is blurred into it.
2. The blurred result is clipped to the node's rounded box and drawn; then the node's own
   background, border, shadow and children paint normally over it.
3. Child glass nodes see the parent glass node's result (nested frosting composes in paint order).
4. Hit testing, focus, accessibility and the scene tree are unaffected: glass is paint only.
5. In high-contrast themes (`HighContrastChanged`, [`os-events-v1.md`](os-events-v1.md)) the engine
   ignores `backdrop-filter` and the stylesheet's opaque fallback applies. This is accessibility
   behavior, not performance degradation, and is not subject to the decision of section 8. A
   `prefers-reduced-transparency` media feature is an open question (section 11).

## 4. Where glass sits in the renderer

Glass needs the renderer to read back what it has already drawn. The Batched2D renderer draws
straight into the frame target and has no offscreen layer, so glass depends on the isolated-layer
seam ([`paint-pass.md`](../design/paint-pass.md), tracked in section 12):

- The paint pass splits the node list into **segments** at each glass node. A segment is drawn into
  the frame target in batch order, as today.
- At a glass node the renderer ends the current render pass, copies the backdrop region (the node's
  box expanded by `3 * sigma`, clamped to the target) into the blur target, runs the blur, and
  resumes the frame target with the blurred region composited through the rounded-box clip.
- On Vulkan the copy is `vkCmdCopyImage` plus layout transitions between render passes; on WebGPU
  it is `copyTextureToTexture`. The RHI gained two operations for this, `copy_target_region`
  (RHI 1.1) and `bind_sampled_target` (RHI 1.3, the sampled view of the copy), plus the render-pass
  break; neither backend gets glass-specific code
  ([ADR-003](../../decisions/ADR-003-thin-rhi-and-batched-2d-renderer.md)). The blur pipeline reads a
  sampled target exactly like a texture, so a backend does not know glass exists.
- Segmentation keeps every tree without a glass node on today's single-pass path, byte for byte.
- The recorded sequence for one glass node is
  `break_pass -> copy_target_region -> [bind pipeline, bind_sampled_target] -> blur pass ->
  bind_sampled_target -> resume_pass -> composite draw -> restore 2D state`. Two pipelines carry it:
  the horizontal pass writes straight-alpha blur data into a pooled target and must not blend; the
  composite draws over the frame target with alpha blending so `opacity` and the rounded clip
  antialias into the frame. Both are created for the frame target's format
  (`vulkan::glass_blur_pipeline_desc`).
- A glass frame renders into an offscreen target even at full quality: the copy source is the frame
  target, and a swapchain image is neither a copy source (RHI 1.1) nor samplable (RHI 1.3). The same
  submission resamples the offscreen frame into the acquired image at content scale 1, so a tree
  without a glass node keeps the direct path.
- The bounded blur-target pool (`glassMaxBlurTargets`) hands out both targets. When it cannot, or the
  renderer has no glass pipelines, the node paints without the effect and `GlassPaintStats::unsupported`
  counts it; nothing blocks, grows without bound or throws on the frame path.

## 5. Blur algorithm

Decision (owner, 2026-11-13): a **separable Gaussian, two passes**, horizontal then vertical, with a
kernel that depends only on `sigma`.

- Kernel taps are computed on the CPU from `sigma` (radius `ceil(3 * sigma)`), normalized to sum 1,
  and uploaded as a small uniform buffer. The shader uses the linear-sampling pairing trick, so the
  tap count is about `radius + 1` instead of `2 * radius + 1`.
- The edge mode is **clamp to edge**, so a glass node at a window border does not darken.
- The blur target is the node's region at **device resolution**, not downsampled. Cost therefore
  grows with area times radius; this is the accepted cost of the decision, bounded by the clamp of
  `glassMaxBlurRadius` and by the optional `glassMaxBackdropArea` (device pixels, default 4 Mpx;
  beyond it the region is blurred at half resolution and upsampled bilinearly, a quality choice, not
  a removal of the effect).
- Blur targets come from a pool of at most `glassMaxBlurTargets` (default 4) textures, sized to the
  largest region of the frame, reused across glass nodes and frames, and retired with frames in
  flight like any render target ([`threading-and-memory.md`](../design/threading-and-memory.md)).
- `saturate`, `brightness` and `contrast` are applied in the vertical pass (a 4x4 color matrix),
  so they add no pass.
- Shaders are GLSL to SPIR-V like every other shader ([ADR-011](../../decisions/ADR-011-shader-authoring-and-cross-compilation.md)),
  one `glass_blur.frag` with a direction push constant, and a WGSL translation through naga.

## 6. Translucent window and OS backdrop

`window-backdrop` and the `Window` option `transparent: true` give the window a translucent client
area. Support by platform for V1:

| Platform | `mica` | `acrylic` | `blur` | `auto` resolves to | Fallback |
|---|---|---|---|---|---|
| Windows 11 (22H2+) | `DWMWA_SYSTEMBACKDROP_TYPE` = Mica | Acrylic | Acrylic | `mica` | solid `background-color` |
| Windows 10 | not available | `SetWindowCompositionAttribute` acrylic (undocumented, off unless `win10Acrylic: true`) | same | `none` | solid `background-color` |
| Linux, Wayland | not available | not available | `ext-background-effect-v1` when the compositor offers it | `blur` if offered, else `none` | solid `background-color` |
| Linux, X11 | not available | not available | `_KDE_NET_WM_BLUR_BEHIND_REGION` on KWin, `_GTK_FRAME_EXTENTS` untouched | `blur` on KWin, else `none` | solid `background-color` |

- The engine reports the resolved material through `WindowMetrics` and the diagnostics
  (`TESSERA_GLASS_WINDOW_UNSUPPORTED`, one warning); a stylesheet can branch with
  `@media (window-backdrop: none)` on the resolved value, so a theme supplies an opaque background
  when no material is available.
- A translucent window needs per-pixel alpha: the swapchain uses a pre-multiplied alpha composite
  mode where the platform offers it. Where it does not, `transparent` degrades to opaque with the
  same diagnostic. The renderer clears to transparent black and the root background is the
  stylesheet's job.
- The OS draws the material; it is never read back, so the in-window `backdrop-filter` of a node
  over a transparent region blurs only what the app painted, not the desktop behind it.
- All Windows calls live in `src/platform/windows` and the X11 and Wayland ones in
  `src/platform/linux`; engine code sees only `WindowMetrics::backdrop` ([ADR-020](../../decisions/ADR-020-window-resize-and-lifecycle.md)).
- macOS (vibrancy) and Android are not V1; the table gains rows for them after V1.

## 7. Dirty rendering and damage

Glass is the case [`dirty-rendering.md`](../design/dirty-rendering.md) lists as unsafe for
region-local redraw, so the rule is explicit:

- A glass node's **paint dependency** is its box expanded by `3 * sigma`. Any paint damage that
  intersects the dependency region dirties the glass node, and the damage rectangle of the frame is
  grown to the glass node's full filtered box (the part that would otherwise show stale blur).
- Damage is computed as the union, then merged by the existing redraw plan; a union covering more
  than `fullRedrawThreshold` becomes a full redraw as usual.
- A change under a glass node that does not move any pixel in its dependency region (an off-screen
  or clipped change) does not dirty it.
- A static glass screen presents nothing after it settles: blur results are not cached across
  frames in V1 (recomputed whenever the node is repainted), so the idle guarantee comes from damage,
  not from a cache.
- Scrolling content under a fixed glass bar redraws the bar's dependency region every frame. That is
  the case the benchmark of section 9 measures first.

## 8. Cost, budget and the decision on limits

Decision (owner, 2026-11-13): **glass is always applied, even when it makes frames slow.** There is
no automatic fallback to a solid background when a frame misses its budget, and adaptive quality
([`performance.md`](../design/performance.md) section 6) does not turn glass off.

What stays, because it bounds the cost without removing the effect:

- The radius clamp (`glassMaxBlurRadius`) and the half-resolution rule above `glassMaxBackdropArea`.
- Adaptive quality may lower `renderScale` as for any content; glass follows the scaled target.
- Frame diagnostics gain `glass_ms` and `glass_backdrop_count` next to the existing
  `render_time_ms`, so a slow frame is attributable. A frame that exceeds its budget with glass
  active increments `missed_deadlines` as always; nothing else changes.

### 8.1 `glass_ms` semantics (pinned 2026-11-14, #244)

`glass_ms` is the **CPU time the frame spent recording its glass work**, not a GPU interval: the wall
time the paint step spends in `Batched2D`'s backdrop draws - region plan, `copy_target_region`, the
two blur pass records and the composite record - summed over the frame's glass nodes. It is 0 on a
frame with no backdrop draw. `glass_backdrop_count` is the number of backdrop draws the frame
emitted, taken from `GlassPaintStats` (`draws` + `unsupported`, so a frame whose composite was
skipped by the bounded pool is still counted). Both are per-frame fields of
`runtime::FrameDiagnosticsSample`; the sample is produced by the app's frame-diagnostics wiring
(`runtime::FrameDiagnostics::record`), which no driver populates yet - the fields are covered by the
accumulator tests until then.

Why CPU record time and not the GPU interval: `FrameTimeSource` timestamps already provide the
frame-level GPU signal ([`performance.md`](../design/performance.md) section 6.1), the blur executes
inside the same submission as the rest of the frame, and a per-node GPU interval would need a query
pool per glass node for a number no V1 decision consumes. It is also the number the CPU benchmark
(#241) measures, so the budget there (`max_glass_paint_ms`) and `glass_ms` describe the same
quantity and can be compared. If a GPU interval ever becomes necessary (section 11), it lands as a
separate `glass_gpu_ms` field beside the CPU one; `glass_ms` is not redefined.
- `powerPolicy: power-save` does **not** disable glass either; the application chooses to drop it
  by changing a class, which is the supported way to build a "reduce effects" setting
  (high contrast of section 3 covers the OS setting).

Consequences accepted with the decision: a screen with many large glass nodes can miss the frame
budget on weak GPUs and on lavapipe. The benchmark records it instead of hiding it.

## 9. Tests and acceptance

- Unit: parsing and unsupported-value diagnostics for the grammar of section 2; clamp and
  `@supports`; kernel normalization and tap count for a range of sigma; segmentation of the paint
  list at glass nodes (a tree without a glass node is byte-identical to today).
- Pixels through the real paint path (`glass_paint_gpu`, `tests/gpu/batched2d/`): a uniform backdrop
  composites to the analytic colour, a red|blue split under a `blur(3px)` node is compared pixel by
  pixel against the CPU `glass_kernel_taps` reference derived from the renderer's own region plan, the
  half-resolution rule above `glassMaxBackdropArea`, the rounded clip (the backdrop shows outside the
  corner radius), the opacity fold (`opacity * blurred + (1 - opacity) * original`) and `blur(0)`
  pass-through are covered, and the run must be validation-clean. The expectations are computed, not
  committed as PNGs: a per-driver golden image would differ between lavapipe and a real GPU, while
  the CPU kernel is the same reference on both.
- Structure through the real sink (`glass_paint_null`, null backend): one glass node records
  break -> copy -> horizontal pass -> resume -> composite with two pooled targets and two sampled
  binds; a renderer without glass pipelines, an exhausted `glassMaxBlurTargets` pool and `blur(0)`
  paint the frame without the effect and count it (`GlassPaintStats::unsupported`); a tree without a
  glass node records exactly one pass with no copy and no sampled bind.
- Damage: a change inside the dependency region repaints the glass box; a change outside does not;
  a static glass screen presents zero frames (the idle test of
  [`performance.md`](../design/performance.md) section 9, test `glass_damage`).
- Window: with a fake HAL, `window-backdrop: auto` resolves per the table of section 6 and the
  fallback media query applies when no material is offered; the real DWM and compositor calls are
  covered by a manual checklist in [`window-chrome-v1.md`](window-chrome-v1.md).
- Benchmark: the CPU benchmark (#241) has a glass scenario (the fixed bar of the reference dashboard
  blurred, one backdrop composite recorded on the null RHI) reporting the paint time against the
  committed `max_glass_paint_ms` budget and the exact `glass_backdrops` count
  ([`tests/golden/perf/cpu_benchmark.txt`](../../tests/golden/perf/cpu_benchmark.txt)). The GPU
  timing is recorded on lavapipe and on the real-GPU lane without a budget in V1.

## 10. Decisions (2026-11-13)

1. **Scope:** in-app `backdrop-filter` plus a translucent window with an OS backdrop on Windows
   and Linux (recommended option).
2. **Algorithm:** separable two-pass Gaussian at device resolution (owner's choice over the
   recommended dual Kawase); cost grows with radius, bounded by the radius clamp and a half
   resolution rule for very large regions.
3. **Limits:** glass is always applied; no fallback to solid, adaptive quality does not disable it
   (owner's choice over the recommended fallback). Slow frames are reported through
   `glass_ms` and `missed_deadlines`.
4. **API:** CSS `backdrop-filter` plus `--glass-*` theme tokens, no separate `material` property
   (recommended option).

## 11. Open questions

- A `prefers-reduced-transparency` media feature and the matching OS event (Windows
  `UISettings.AdvancedEffectsEnabled`, GNOME has none).

- Dual Kawase or downsampled blur as an optional mode for weak GPUs, if the benchmark of section 9
  shows the Gaussian cost is a problem on real hardware.
- Lens/refraction as a `backdrop-filter` function (Liquid Glass look).
- Caching the blurred backdrop across frames when only the glass node's own content changes.
- `backdrop-filter` on a node inside an isolated layer or a shared-element snapshot
  ([`element-transitions-v1.md`](element-transitions-v1.md)): the backdrop is the layer's own
  content until layers can sample their parent.

## 12. Work items

Tracked as issues in the V1 milestones: #242 (renderer seam: render-pass break, RHI
`copy_target_region`, offscreen pool - landed); #254 (RHI 1.3 sampled view of a render target:
layout transitions, sampler state, WebGPU twin, the first glass pixel test - landed); #244
(`backdrop-filter`, blur shaders, damage rules, diagnostics, benchmark scenario, and the renderer
wiring on top of #242 and #254 - landed: `Batched2DRenderer::draw_backdrop` executes the composite
from the real paint path, `glass_paint_gpu`/`glass_paint_null` are the pixel and structure proofs,
and `glass_ms` is pinned in section 8.1); #243 (translucent window and OS backdrop on Windows and
Linux - landed).

Still open, not tracked as a V1 issue: the app frame-diagnostics wiring that produces
`FrameDiagnosticsSample` (and therefore `glass_ms`/`glass_backdrop_count`) on a driver frame, per
section 8.1.
