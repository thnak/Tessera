# Outer `box-shadow` clipping (issue #461)

Status: implemented (`src/ui/paint/paint_shadow.cpp`, `batched2d.frag` / `batched2d.wgsl`,
`src/rendering/gpu/batched2d_renderer.cpp`). Paint-pass context: [paint-pass.md](../paint-pass.md)
section 5; shader AA notes: [antialiasing.md](../antialiasing.md) section 1; vertex/push-constant
layout: [pipelines.md](pipelines.md) sections 4 and 6.

## 1. The rule

CSS paints an **outer** shadow only outside the border box: the shadow is clipped inside the
border box, which for a rounded element is the *rounded* border box (CSS Backgrounds 3 section
7.1.1). An `inset` shadow is painted inside by definition and is not part of this rule.

Two consequences the engine had wrong:

- A transparent (or translucent) box with a shadow showed the shadow through its own fill. In
  Chrome the interior is the page; the engine painted the full Gaussian there, so the interior was
  the shadow colour blended over the page (a grey patch).
- The clip follows the rounded silhouette, so the corner triangles of a rounded box - inside the
  box rectangle, outside the corner arc - **do** keep the shadow. Chrome paints 244 on 255 white
  at the corner triangle of a 48x48 box with radius 12 and `0 2px 8px rgba(0,0,0,.3)`; a
  rectangular carve would leave 255 there and be wrong.

## 2. One shape per shadow (the batched 2D renderer)

`paint_box_shadows` emits one `PaintSink::fill_box_shadow` call per outer shadow - sharp
(`sigma = 0`) or blurred - when the sink reports `supports_box_shadow()`. The draw carries the
shadow's own rounded box, the node's rounded border box as `carve`, the colour, the corner radius
and `sigma`.

The fragment stage evaluates two rounded-box signed distances: the shadow's, whose coverage is
the Gaussian `0.5 * (1 - erf(d / (sigma * sqrt 2)))` (or the AA `smoothstep` when `sigma = 0`),
and the carve box's, whose coverage is 0 inside. The carve radius is the shadow radius minus the
uniform inset, which is exact because an engine `border-radius` is one length, and it stays inside
the clamp of a shadow radius that the shadow box already clipped.

The element box reaches the fragment stage as a **vertex attribute** (`Vertex2D::carve`, `Float4`,
location 7: centre offset in quad-local px and half size), not as push constants:

- the push-constant block is 112 of the 128 bytes Vulkan guarantees and is *per draw batch*, so the
  carve would both exhaust that budget and split a batch around every shadow (the batch key is
  texture, scissor and folded clip state);
- the carve is per primitive, so the vertex stream is where it belongs;
- the cost is 16 bytes per vertex, 44 -> 60 bytes, on every primitive. That growth is deliberate
  and is why `Vertex2D` still has one stable prefix: it was 44 bytes for nine slices.

## 3. Flattening sinks (the CPU reference and anything without the quad)

`CpuPaintSink` and every other sink that does not report `supports_box_shadow()` gets the
deterministic stack of concentric fills for a blurred shadow (unchanged falloff maths), but each
layer is emitted as **up to four axis-aligned strips around the border box** instead of one fill:
the strips share edges with the carve box exactly, so a translucent layer is never composited
twice. That sink has no inverse clip, and pushing a rounded clip around the shadow would unbalance
the paint cache's recorded push/pop pair.

The strips are rectangles, so a rounded layer's corner triangles are left unpainted. That is the
conservative direction - never a shadow where CSS paints nothing - and it is the same bounds
approximation the CPU canvas already makes for a rounded fill
([antialiasing.md](../antialiasing.md) section 2.2). The CPU golden therefore pins "nothing inside
the border box" more strongly than the GPU path, and the GPU path pins the corner triangle.

## 4. Tests and baselines

| Test | What it proves |
|---|---|
| `paint_shadow` | unit: every layer is carved (no emitted rect touches the border box), the one-shape draw carries the carve box, the radius and `sigma`; the blurred stack keeps its Gaussian profile and its reduced-effects cap |
| `paint_shadow_cpu` | pixels on the deterministic CPU canvas: the centre and the inside edges of a transparent box are the page colour, the shadow below and beside it is not, round or square |
| `shadow_clip_gpu` | real Vulkan/lavapipe readback through the renderer *and* the paint pass: transparent centres are page colour, the corner triangle keeps a light shadow, the shadow band still paints (skips without a device) |
| `cpu_wide_golden` `shadows` | the committed CPU baseline `tests/golden/baseline/cpu_wide/shadows.png`: transparent square, transparent rounded, opaque card |
| `batched2d_shadow_golden` | the lavapipe baseline `tests/golden/baseline/batched2d/shadow_clip.png` for `batched2d_poc --shadow-clip --width 320 --height 200` |
| `gallery_golden_form_plan_mid` | the form's plan segment fades in from a transparent base again (`samples/gallery/form.module.css`) |

Baselines are generated on the reference renderer only (ADR-009): the CPU ones from the CPU path,
`shadow_clip.png` on a Linux lavapipe lane:

```bash
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  ./build/dev/tools/poc/batched2d/batched2d_poc --backend vulkan --golden --shadow-clip \
    --width 320 --height 200 --out tests/golden/baseline/batched2d/shadow_clip.png
./build/<dir>/tests/gpu/cpu_wide_golden_tests --update-goldens   # shadows.png
```

The page-level check is a browser probe, developer-run like the other preview probes:

```bash
node scripts/ci/preview_browser_shadow.mjs --site build/pages [--shot out.png]
```

It compiles a transparent box with `0 2px 8px rgba(0,0,0,.3)` in the studio, draws it on WebGPU and
reads the element screenshot: the box centre must be the preview page colour (measured 255 in
Chrome, 255 in the engine; 178 before this change), and the shadow band below must still be
shaded.
