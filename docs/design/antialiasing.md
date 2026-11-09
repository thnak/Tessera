# Anti-aliasing

Status: B1 (renderer-core phase of [Big Step B](../../AGENTS.md#big-steps-group-work-dont-micro-step)) has **partial** AA (anti-aliasing): analytic SDF (signed distance field) AA for rounded rectangles,
grayscale coverage from the glyph atlas for text, and an opt-in 4x MSAA color-target resolve path
(§2.1). There is no MSAA 8x, no FXAA (fast approximate AA)/TAA (temporal AA), and no vector-path AA
beyond what the path backend rasterizes.

## 1. What exists today

`src/rendering/backend/vulkan/shaders/batched2d.frag` evaluates a rounded-box signed distance
and blends the edge:

```glsl
float d = sd_round_box(v_local, v_half_size, v_radius);
alpha = 1.0 - smoothstep(-pc.aa_radius, pc.aa_radius, d);
color.a *= alpha;
```

- `Batched2DConfig::enable_aa` (default `true`) controls the radius.
- `Batched2DConfig::golden_mode` (deterministic-test mode; see "Golden" in the [glossary](../reference/glossary.md)) forces `aa_radius = 0.0`, producing hard, pixel-stable edges
  (`--golden` on the demo).
- No `discard`; transparent fragments write alpha 0 and disappear through alpha blending.
  This avoids requiring `shaderDemoteToHelperInvocation`.
- AA is applied to SDF rounded rects (including borders built from them). It is not a
  post-process or a multisample pass.
- **Rounded clips are hard-edged.** The batched2d fragment stage multiplies alpha to 0 outside each
  rounded clip (also without `discard`), so the clip boundary itself has no coverage AA: at 1× DPI a
  rounded clip corner and its straight edges are aliased until MSAA lands. The clip test uses the
  same pixel-center rule as rasterization, so it is deterministic in golden mode and never writes
  outside the clip. MSAA (which would smooth both geometry and clip edges) remains unimplemented.

## 2. What is not covered

| Area | Current state |
|---|---|
| Solid quads | Axis-aligned; fractional edges can alias; no geometric AA unless a 4x target is used |
| Textured quads | Texture filtering only (nearest/linear); no geometric AA unless 4x |
| Text/glyphs | Text stack/atlas is B2 ([`text.md`](text.md)); expected FreeType/atlas AA + subpixel policy |
| Vector paths/strokes | [ADR-004](../../decisions/ADR-004-buy-vector-path-rasterization.md) (ThorVG/Skia); no path rasterizer yet |
| Lines/circles/arbitrary shapes | Only rounded-box SDF; no general SDF |
| Rounded clip edges | Hard pixel-center coverage mask (no clip AA); uses alpha 0, not `discard` |
| MSAA | 4x color targets implemented (opt-in); 8x/per-format/depth MSAA deferred |
| FXAA/TAA/post-process | Not implemented |
| Transforms/DPI (device pixel ratio, see "DPI scale" in the glossary) | AA radius is fixed in local units (1.0); scaled/rotated quads may get a different screen-space width |
| Golden coverage | CPU goldens stay 1-sample; 4x coverage is a deterministic real-GPU readback test |

## 2.1 MSAA 4x (implemented, opt-in)

- `RenderTargetDesc::sample_count = 4` creates a multisample render image plus a 1-sample resolve
  image; dynamic rendering resolves with `VK_RESOLVE_MODE_AVERAGE_BIT` at end of pass
  (`storeOp=DONT_CARE` for the multisample image). Readback/sampling observe the resolved image.
- `PipelineDesc::sample_count = 4` must match the target; `Batched2DConfig::sample_count = 4` opts
  the renderer in. A mismatch is rejected explicitly (never silently degrade).
- `Device::supports_sample_count(format, 4)` is the capability gate; on failure creation reports the
  requested format/count and tests skip (no silent 1x fallback).
- What benefits: non-SDF geometry — rotated/scaled solid quads, path/triangulated edges and thin
  strokes, and anything else with hard geometric edges. SDF rounded rects already get analytic AA
  and coverage text already carries atlas coverage; both keep working unchanged on a 4x target (they
  don't need MSAA, and their resolved output equals the 1-sample output when geometry is
  sample-aligned).
- Not anti-aliased by MSAA: clip edges (the clip mask is a hard coverage test, so rounding the
  clip stack does not smooth them), post-process effects, and any alpha-blended overlap that is
  already resolved per-sample.
- Memory: ~5x the 1-sample color bytes for a 4x target (four color samples + the resolve image) plus
  resolve bandwidth; see [`gpu/resources.md`](gpu/resources.md) §4. MSAA is a per-target opt-in
  (quality/power tradeoff), not a global default.
- Golden policy: `batched2d_cpu_golden` stays 1-sample and byte-exact. The CPU canvas and current
  lavapipe corpus do not include a 4x target, so no 4x image golden is added; the 4x path is covered
  by a deterministic real-GPU readback test (edge-differ/interior-equal/repeat-identical). A
  lavapipe 4x golden would be CI-only (software Vulkan) if added later.
- Deferred: MSAA 8x, per-format sample-count sets beyond 1/4, depth/stencil MSAA, other resolve
  modes, HDR/wide-gamut interactions, and tying sample count to a quality/power policy.

## 3. Quality and configuration (proposed)

AA is a quality/power knob; it must not change layout. Proposed config:

```json
"renderer": {
  "aaMode": "sdf",
  "aaRadius": 1.0,
  "msaaSamples": 1,
  "textAa": "grayscale",
  "pathAa": "backend"
}
```

- `aaMode`: `off`, `sdf` (V1, first-release scope, see [AGENTS.md rule 8](../../AGENTS.md#non-negotiables)), `auto`; `msaa`/`post` have no release assigned.
- `aaRadius`: 0.5..2.0 local units; `0` = off (golden).
- `msaaSamples`: 1/2/4/8; increases bandwidth/memory; not V1.
- `textAa`: `grayscale`/`subpixel`; golden disables subpixel.
- `pathAa`: delegated to the path backend ([ADR-004](../../decisions/ADR-004-buy-vector-path-rasterization.md)).
- Golden/deterministic mode pins `aaMode=sdf`/radius 0 or a fixed radius; user tunables are
  ignored (see [`buffer-config-v1.md`](../specs/buffer-config-v1.md) for tunable rules).

These are proposals; no config is implemented in B1.

## 4. Design options

1. **Analytic SDF AA (current).** Cheap, stable, good for rounded rects; width should ideally
   use screen-space derivatives (`fwidth`) so scale/rotation/DPI keep ~1 px.
2. **MSAA.** Correct for arbitrary geometry; expensive in bandwidth/memory; requires
   multisample targets/resolve and interacts with batching/alpha.
3. **Post-process AA (FXAA/SMAA/TAA).** Full-scene; can blur text/UI; TAA needs history and
   motion vectors (not suitable for static UI without care).
4. **Text/path AA.** Text uses FreeType/atlas AA; paths use the chosen backend's AA. Both need
   their own golden corpus and gamma/color policy.

Recommendation for V1: keep analytic SDF AA for rounded rects, add text/path AA with their
stacks, and defer MSAA/post-process until a concrete quality need appears. If MSAA is added,
make it a quality/power option, not the default.

## 5. Screen-space correctness

The current radius is in local space. With a scaled transform, the screen-space edge width
changes. A proposed change (no release assigned): SDF AA should use derivatives:

```glsl
float w = max(fwidth(d), 1e-4);
alpha = 1.0 - smoothstep(-w, w, d);
```

This keeps a ~1 px edge regardless of scale/rotation/DPI. It must be validated on lavapipe (Mesa software Vulkan driver) and
real GPUs before replacing the fixed radius.

## 6. Color and gamma

- Alpha blending currently assumes a simple alpha blend; sRGB/gamma policy is not scheduled and no decision has been made (see "B2 / known limits" in [`renderer.md`](renderer.md) and [ADR-001](../../decisions/ADR-001-vulkan-1-3-baseline.md)).
- Text AA in linear vs sRGB space can look different; pin the policy in golden mode.
- Overlapping translucent edges need a deterministic blend order (painter's order).

## 7. Tests

- Golden/edge scenes: rounded rects at 0°, 45°, scaled 0.5×/2×; thin borders; radii near 0.
- DPI/scale: 1.0/1.25/1.5/2.0; fractional positions.
- Overlap: translucent edges over backgrounds and textures.
- Text/path AA: once implemented, fixed fonts/DPI and bit-exact software goldens.
- Determinism: same scene/AA config -> same pixels; golden mode ignores user AA tunables.
- Performance: AA on/off frame time; MSAA bandwidth if added.

## 8. Open questions

- Fixed local radius vs `fwidth` screen-space radius.
- Whether MSAA is ever needed for a 2D UI; cost/benefit.
- Text AA: grayscale vs subpixel; gamma-correct blending.
- Path AA backend (ThorVG/Skia) and parity with SDF/text.
- AA as a user tunable (presets vs raw radius) and power impact.
- Golden tolerance for text/path AA across software backends.
