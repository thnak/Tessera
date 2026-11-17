# Display-aware rendering (text and non-text anti-aliasing)

Status: **proposal; S1 landed** (`AaPolicy` resolver in `include/tessera/ui/scale/aa_policy.hpp`
and `hal::PixelGeometry`, nothing consumes them yet; S2-S8 are not implemented). Extends [`antialiasing.md`](antialiasing.md) and
[`text.md`](text.md). Decisions D2/D6 in
[`rendering-production-plan.md`](rendering-production-plan.md) stay in force unless a numbered
decision in section 8 says otherwise.

## 1. Problem

Anti-aliasing quality today does not depend on the display. The shader edge width is a fixed
0.5 local unit, text is grayscale coverage rasterized at the caller's `pixel_size`, rounded clips
are hard-edged, and hairlines are not snapped to device pixels. The only display input that
reaches rendering is the quantized render scale (`scale::quantize_render_scale`, ladder
1.0/1.25/1.5/2.0, [`window-resize.md`](window-resize.md) section 2.3), and for text it only keys
the atlas. Result: edges are 1 device pixel wide at 1x only by luck, text uses the same recipe at
96 dpi and 192 dpi, and a monitor change (`WM_DPICHANGED`) changes size but not quality policy.

Goal: derive each anti-aliasing decision from *display facts* the HAL already normalizes, keep
layout independent of them, and keep golden mode pinned.

## 2. Display facts

| Fact | Source | Status |
|---|---|---|
| Device scale (ladder step) | `WindowMetrics::dpi_scale` -> `quantize_render_scale` | exists |
| Pixel geometry (RGB/BGR stripe, H/V, none/unknown) | HAL, per display | **new** `PixelGeometry` field |
| Screen rotation / orientation | `WindowMetrics` orientation | exists, unused by AA |
| Surface opacity (opaque window vs translucent/glass) | window config | exists |
| Node transform (translate-only, scale, rotate), layer opacity | scene | exists |

Rules: a fact change is a *between-frames* event like `set_render_scale()`. No fact is read on the
frame path except through an already-resolved per-window `AaPolicy` value. Layout never reads
them (AA must not change layout, `antialiasing.md` section 3).

`AaPolicy` (plain data, resolved once per window and per fact change):
`{ render_scale, text_mode, hinting, subpixel_position, edge_mode, clip_aa, msaa }`.
Golden mode replaces it wholesale with a fixed value (section 7).

## 3. Text

### 3.1 Raster size follows the device (T1)

Rasterize at `css_px * render_scale` device pixels instead of the caller's `pixel_size` alone.
The atlas key already carries the scale; the missing piece is that the pixel size handed to
FreeType and to the metrics provider is the device size, while layout measures in CSS px from
design metrics so line breaks do not move between scales. Advances are rounded after scaling.

### 3.2 Hinting by scale (T2)

| Render scale | FreeType load target | Why |
|---|---|---|
| 1.0 | `FT_LOAD_TARGET_LIGHT` (vertical-only autohint) | stems land on rows, shapes stay true |
| 1.25 / 1.5 | light, vertical only | partial benefit, no horizontal distortion |
| 2.0 | `FT_LOAD_NO_HINTING` | density hides it; shapes identical across vendors |

Today `FT_LOAD_DEFAULT` follows the font's own hints. Golden mode keeps one fixed target.

### 3.3 Subpixel positioning (T3)

Glyph origins carry a horizontal fraction quantized to 1/4 px; the atlas key gains a 2-bit
`x_subpixel`. Enabled when `render_scale >= 1.25` or the policy says so; off at 1.0 (hinted stems
would blur) and always off in golden mode (`text.md`, determinism). Bound: 4x entries per glyph,
counted against the existing bounded atlas budget; eviction rules unchanged.

### 3.4 LCD subpixel text (T4, optional)

Modes: `grayscale` (today) and `lcd` (3 coverage channels). LCD is chosen only when **all** hold:
`PixelGeometry` is a known horizontal stripe, the window is opaque, the run has a translate-only
transform and effective opacity 1, and nothing translucent, glass, or animated lies beneath it.
Otherwise the run uses grayscale; fringes are never mixed into a run. Rasterize with
`FT_RENDER_MODE_LCD` (FreeType's Harmony renderer; verify the pinned version supports it and that
no patent-encumbered filter is needed) into an RGB coverage atlas page. Compositing needs a
per-channel blend: dual-source blending where `dualSrcBlend` is supported; reading the destination
in the shader is not allowed, so the fallback is grayscale. Rotated screens and vertical stripes
are grayscale in V1.

### 3.5 Contrast and gamma (T5)

Coverage blends in sRGB-encoded space (D2), so light-on-dark text looks thin and dark-on-light
heavy. Proposal: a per-run coverage adjustment from foreground luminance, a 256-entry LUT applied
in the atlas upload path, not a shader branch. **Needs an owner decision** (section 8): it touches
D2 only for text coverage, not for fills.

### 3.6 What stays

An SDF/MSDF atlas stays P1-optional (D6): it is a scaling tool, not a display-quality tool.
Platform rasterizers (DirectWrite/CoreText) stay rejected (ADR-006, determinism).

## 4. Everything that is not text

The same facts apply to the rest of the renderer; each row is independent of text.

| Component | Today | Display-aware change | Slice |
|---|---|---|---|
| SDF rounded rect and border | fixed `aa_radius` 0.5 local | edge width from `fwidth(d)`, clamped to [0.5, 1.0] device px; golden keeps radius 0 | S2 |
| Hairlines / 1 CSS px borders | width follows scale, not snapped | snap to whole device px, minimum 1; reuse `pixel_snap` (`src/ui/layout/pixel_snap.cpp`) at paint time | S3 |
| Rounded clip edge | hard pixel-center mask | coverage from the clip's SDF with the same `fwidth` width; clip stack multiplies coverage | S4 |
| Axis-aligned quads, fractional edges | can alias | snap at paint (S3); only transformed quads stay soft | S3 |
| Images / textured quads | nearest or linear by flag | filter by scale: nearest at aligned 1:1, linear otherwise, mips when downscaling; decode at display size x DPR (#14) | S8 |
| Vector paths | no rasterizer yet | rasterize at device scale, cache per ladder step; strokes clamp to 1 device px minimum | with ThorVG (D1) |
| Offscreen layers / glass | layer at 1x unless told | layer targets allocated at the window's render scale and keyed by it | with layers |
| MSAA 4x | per-target opt-in | policy picks it only for non-SDF, non-text geometry and scale < 2.0; never default | S8 |
| FXAA / TAA / post AA | out of V1 | unchanged, out | - |

Rule: SDF AA plus pixel snapping carries the 2D UI; MSAA is the exception, not the answer. Clip AA
(S4) is display-independent in cause but shares the `fwidth` helper, so it ships with S2.

## 5. Configuration

`renderer.textAa`: `auto` (default, the policy above) | `grayscale` | `subpixel`.
`renderer.aaMode` / `aaRadius` / `msaaSamples` as in `antialiasing.md` section 3. Presets only, no
per-node radius (`open-questions.md`, antialiasing#5). The power policy
([ADR-019](../../decisions/ADR-019-power-aware-runtime.md)) may force `grayscale` and `msaa=1`. A
user setting never overrides golden mode.

## 6. Slices

| Slice | Content | Tests |
|---|---|---|
| S1 | `AaPolicy` resolver + HAL `PixelGeometry` (Unknown default; Win32/X11 fill later) | policy table unit test with fake displays |
| S2 | `fwidth` edge in `batched2d.frag` for SDF and border | edge-width probe at 0.5x/1x/2x/45 deg; golden unchanged |
| S3 | paint-time snap of hairlines and borders | 1.25/1.5 scale fixtures, 1-device-px assertion |
| S4 | rounded-clip coverage AA | clip corner has intermediate alpha; nothing written outside the clip |
| S5 | text T1 + T2 (device raster size, hinting by scale) | metrics unchanged across scales; atlas key test |
| S6 | text T3 (subpixel positioning) | golden mode unaffected; bounded atlas growth |
| S7 | text T4 + T5 (LCD, contrast LUT), after the section 8 decisions | LCD only on eligible runs; fallback test |
| S8 | image filter by scale, MSAA policy hook | filter selection table test |

S1 details: `Auto` text mode resolves to grayscale until decision 1 below is made
(`kAutoAllowsLcd`); `WindowMetrics::pixel_geometry` and `DisplayInfo::pixel_geometry` default to
`Unknown` and no backend fills them yet; the resolver is pure and covered by `aa_policy`.

Order: S1 first (everything reads it), then S2 + S4 together, S3, S5, S6. S7 is gated on section 8.

## 7. Determinism and goldens

Golden mode pins `AaPolicy` to `{ scale 1.0, grayscale, fixed hinting, no subpixel positioning,
edge radius 0, clip AA off, msaa 1 }` and ignores all facts and user settings. New behavior gets
its own corpus only where it can be deterministic: CPU goldens stay tolerance 0
(`antialiasing.md` section 2.2); LCD and `fwidth` edges are covered by lavapipe probes with an
explicit tolerance, never by real-GPU goldens.

## 8. Decisions needed

1. Ship LCD text in V1, or grayscale only with T1-T3? (Recommendation: T1-T3 in V1, LCD after.)
2. Adopt the text contrast LUT (T5), and does it count as a D2 amendment?
3. Windows: take `PixelGeometry` from the OS ClearType setting, or detect per display only?
4. Clip AA (S4) in V1, or accept hard rounded clips until MSAA? (Recommendation: S4 in V1.)
