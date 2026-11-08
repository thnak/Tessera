# Anti-aliasing

Status: B1 has **partial** AA. It is analytic SDF AA for rounded rectangles only; there is no
MSAA/FXAA/TAA, no text AA yet, and no vector-path AA yet.

## 1. What exists today

`src/rendering/backend/vulkan/shaders/batched2d.frag` evaluates a rounded-box signed distance
and blends the edge:

```glsl
float d = sd_round_box(v_local, v_half_size, v_radius);
alpha = 1.0 - smoothstep(-pc.aa_radius, pc.aa_radius, d);
color.a *= alpha;
```

- `Batched2DConfig::enable_aa` (default `true`) controls the radius.
- `Batched2DConfig::golden_mode` forces `aa_radius = 0.0`, producing hard, pixel-stable edges
  (`--golden` on the demo).
- No `discard`; transparent fragments write alpha 0 and disappear through alpha blending.
  This avoids requiring `shaderDemoteToHelperInvocation`.
- AA is applied to SDF rounded rects (including borders built from them). It is not a
  post-process or a multisample pass.

## 2. What is not covered

| Area | Current state |
|---|---|
| Solid quads | Axis-aligned; fractional edges can alias; no geometric AA |
| Textured quads | Texture filtering only (nearest/linear); no geometric AA |
| Text/glyphs | Text stack/atlas is B2; expected FreeType/atlas AA + subpixel policy |
| Vector paths/strokes | ADR-004 (ThorVG/Skia); no path rasterizer yet |
| Lines/circles/arbitrary shapes | Only rounded-box SDF; no general SDF |
| MSAA | No multisample pipeline/sample count |
| FXAA/TAA/post-process | Not implemented |
| Transforms/DPI | AA radius is fixed in local units (1.0); scaled/rotated quads may get a different screen-space width |
| Golden coverage | No pixel golden asserts AA quality yet; B1 tests check draws/vertices |

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

- `aaMode`: `off`, `sdf` (V1), `auto`; later `msaa`/`post`.
- `aaRadius`: 0.5..2.0 local units; `0` = off (golden).
- `msaaSamples`: 1/2/4/8; increases bandwidth/memory; not V1.
- `textAa`: `grayscale`/`subpixel`; golden disables subpixel.
- `pathAa`: delegated to the path backend (ADR-004).
- Golden/deterministic mode pins `aaMode=sdf`/radius 0 or a fixed radius; user tunables are
  ignored (see `buffer-config-v1.md` for tunable rules).

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
changes. Future SDF AA should use derivatives:

```glsl
float w = max(fwidth(d), 1e-4);
alpha = 1.0 - smoothstep(-w, w, d);
```

This keeps a ~1 px edge regardless of scale/rotation/DPI. It must be validated on lavapipe and
real GPUs before replacing the fixed radius.

## 6. Color and gamma

- Alpha blending currently assumes a simple alpha blend; sRGB/gamma policy is a B2 follow-up.
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
