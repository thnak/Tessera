# Display-aware rendering (text and non-text anti-aliasing)

Status: **S1-S6 and S8 landed, plus the T5 contrast LUT, the window integration in section 6, and
the rasterizer and atlas half of S7 (T4); LCD text is not composited yet.** Landed: the `AaPolicy`
resolver and `hal::PixelGeometry` (S1),
screen-space SDF edges and anti-aliased rounded clips in `batched2d.frag`/`.wgsl` (S2, S4), border
widths snapped to device pixels in the paint pass (S3), glyph hinting by scale, quarter-pixel glyph
origins and text rasterized at `css_px * render_scale` (S5 T1/T2, S6), the MSAA scale rule in the
resolver (S8), the text contrast LUT adopted as a D2 amendment (T5, section 3.5), and the window
integration: `AppWindow` resolves and applies the policy, sets `PaintContext::device_scale`, and
pins the golden policy in golden mode (section 6). S8 also landed the image filter-by-scale rule
(nearest at aligned 1:1, linear otherwise; the mip case is blocked on RHI mip support, section 6)
and consumes the resolver's MSAA decision in the window's presentation. S7 landed in two halves:
the Windows backend fills `PixelGeometry` from the user's ClearType configuration (section 2), the
only subpixel source the OS has, and says honestly that it is desktop-wide rather than per-monitor;
and `GlyphAaMode` gives the rasterizer and atlas an LCD mode (`FT_RENDER_MODE_LCD`, three coverage
samples per pixel, the mode in the atlas identity, RGB upload, and a quad one third of the page
extent, section 3.4.1). Not landed: **LCD compositing** - the per-run eligibility predicate, the
renderer's pipeline axis and the paint-seam fact (section 3.4.1 step 3), so every run still
composites through the grayscale pipeline; the mip-chain downscale case (needs `TextureDesc` mip
levels and backend chain generation); and the X11/Wayland `PixelGeometry` sources (both still report
`Unknown`). Extends
[`antialiasing.md`](antialiasing.md) and [`text.md`](text.md). Decisions D2/D6 in
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
| Pixel geometry (RGB/BGR stripe, H/V, none/unknown) | HAL, per display | exists; Win32 fills it from ClearType (desktop-wide, see below) |
| Screen rotation / orientation | `WindowMetrics` orientation | exists, unused by AA |
| Surface opacity (opaque window vs translucent/glass) | window config | exists |
| Node transform (translate-only, scale, rotate), layer opacity | scene | exists |

Rules: a fact change is a *between-frames* event like `set_render_scale()`. No fact is read on the
frame path except through an already-resolved per-window `AaPolicy` value. Layout never reads
them (AA must not change layout, `antialiasing.md` section 3).

**Win32 cannot report a per-monitor stripe.** The Desktop Window Manager never exposes a monitor's
subpixel order: there is no `HMONITOR`, `DISPLAYCONFIG_*` or DXGI query for it, because the panel's
stripe layout is not a property Windows models. What the OS does expose is the user's ClearType
configuration - `SPI_GETFONTSMOOTHING`, `SPI_GETFONTSMOOTHINGTYPE` and
`SPI_GETFONTSMOOTHINGORIENTATION` (the same values mirrored under
`HKCU\Control Panel\Desktop\FontSmoothing{,Type,Orientation}`) - which is *per user and
desktop-wide*, not per display. `hal::clear_type_geometry` maps it honestly: ClearType with the RGB
orientation is `RgbHorizontal`, ClearType with BGR is `BgrHorizontal`, smoothing off or greyscale
("standard") smoothing is `None` (the OS will not blend subpixel coverage, so there is no stripe in
use), and a failed or unrecognised read is `Unknown`. Every `DisplayInfo` therefore carries the same
value, and it is the value the OS will actually rasterize text with on any of the monitors. A
mixed-panel desktop (an RGB laptop panel next to a BGR monitor) cannot be described by this API;
reporting one desktop-wide answer is the honest option, and inventing a per-display one would be a
fabrication. Vertical stripes are unreachable: ClearType only offers horizontal RGB/BGR. X11 and
Wayland report `Unknown` in V1.

`AaPolicy` (plain data, resolved once per window and per fact change):
`{ render_scale, text_mode, hinting, subpixel_position, edge_mode, clip_aa, msaa }`.
Golden mode replaces it wholesale with a fixed value (section 7).

## 3. Text

### 3.1 Raster size follows the device (T1)

Rasterize at `css_px * render_scale` device pixels instead of the caller's `pixel_size` alone.
The atlas key already carries the scale; the missing piece is that the pixel size handed to
FreeType and to the metrics provider is the device size, while layout measures in CSS px from
design metrics so line breaks do not move between scales. Advances are rounded after scaling.

Landed: `TextRenderer::draw` keeps its CSS-space contract but rasterizes each glyph at
`round(css_px * render_scale)` device pixels, keys the atlas by that device size, and converts the
quad back into CSS units - `pen + bearing_device / render_scale`, `bitmap_size / render_scale`,
`baseline - y_offset - bearing_y_device / render_scale`. The pen walk, `run.advance` and every
metric the layout engine sees stay in CSS px. Glyph origins snap in device space (a hinted stem must
land on a device pixel), which at 1.0 is exactly the previous whole-pixel snap.

### 3.2 Hinting by scale (T2)

| Render scale | FreeType load target | Why |
|---|---|---|
| 1.0 | `FT_LOAD_TARGET_LIGHT` (vertical-only autohint) | stems land on rows, shapes stay true |
| 1.25 / 1.5 | light, vertical only | partial benefit, no horizontal distortion |
| 2.0 | `FT_LOAD_NO_HINTING` | density hides it; shapes identical across vendors |

Today `FT_LOAD_DEFAULT` follows the font's own hints. Golden mode keeps one fixed target. Landed:
the resolver's `hinting` field reaches the rasterizer through `TextRenderer::set_aa_policy`, and
golden mode keeps `Font` (the font's own hints), so a renderer that never applies a policy draws as
before.

### 3.3 Subpixel positioning (T3)

Glyph origins carry a horizontal fraction quantized to 1/4 px; the atlas key gains a 2-bit
`x_subpixel`. Enabled when `render_scale >= 1.25` or the policy says so; off at 1.0 (hinted stems
would blur) and always off in golden mode (`text.md`, determinism). Bound: 4x entries per glyph,
counted against the existing bounded atlas budget; eviction rules unchanged.

### 3.4 LCD subpixel text (T4, optional)

Landed in two slices (section 3.4.1): the rasterizer and atlas half, and the compositing half that
is still to come. The Windows backend fills `PixelGeometry` (section 2), but the resolver keeps
`Auto` on grayscale until compositing exists (`kAutoAllowsLcd == false`), so every window still
resolves `Grayscale` and the eligibility rules below are the compositing slice's contract.

Modes: `grayscale` (the default) and `lcd` (3 coverage channels). LCD is chosen only when **all**
hold: `PixelGeometry` is a known horizontal stripe, the window is opaque, the run has a
translate-only transform and effective opacity 1, and nothing translucent, glass, or animated lies
beneath it. Otherwise the run uses grayscale; fringes are never mixed into a run. Rasterize with
`FT_RENDER_MODE_LCD` into an RGB coverage atlas page - verified against the pinned FreeType
(VER-2-13-3) and needing no patent-encumbered filter, because that build has
`FT_CONFIG_OPTION_SUBPIXEL_RENDERING` off and therefore applies no LCD filter (section 3.4.1).
Compositing needs a per-channel blend: dual-source blending where `dualSrcBlend` is supported;
reading the destination in the shader is not allowed, so the fallback is grayscale. Rotated screens
and vertical stripes are grayscale in V1.

The Win32 fact behind the first condition is one desktop-wide ClearType answer, not a per-monitor
one (section 2). A mixed-panel desktop therefore takes the same decision for every monitor: LCD text
uses the stripe order the user selected, which is what the OS itself rasterizes with. A future
backend with a real per-panel query (DXGI `DISPLAYCONFIG_*` output technology does not report it
today) can fill a different value per `DisplayInfo` without changing this rule.

#### 3.4.1 What the LCD half still needs, and why it is not one landing

The shader tooling is *not* the blocker, and that was verified rather than assumed: the manifest
schema makes `sources.webgpu` optional for an `element` and requires it only for a
`view-transition-effect` ([`shader-manifest-v1.md`](../specs/shader-manifest-v1.md) section 3;
`src/schema/src/shader_manifest_rules.cpp:195`), the cross lane names each shader explicitly
(`tools/shaders/cross/CMakeLists.txt:9` and `:15`) and its workflow builds and inspects only those
two ids (`.github/workflows/linux.yml:261`), so a Vulkan-only `batched2d_lcd` entry with no cross
target is ignored there; `scripts/ci/validate_wgsl.py` validates authored `*.wgsl` files it finds
under `src/`, so a shader with no WGSL source is invisible to the `wgsl-validation` lane; and the
Wasm/WebGPU lane resolves programs through `webgpu::ShaderRegistry` by `shader_id`, so an id only
the Vulkan pipeline descriptor names is never looked up. That was also checked empirically: a
throwaway manifest carrying a Vulkan-only `batched2d_lcd` entry passes `tessera_shader_manifest_check`
(the same entry declared as a `view-transition-effect` is rejected at `$.shaders[n].sources.webgpu`,
which is the only place the field is mandatory), and a probe fragment shader with
`layout(location = 0, index = 1) out vec4` compiles with `glslc`, validates under
`glslangValidator`/`spirv-val`, and disassembles with `Index 1` on that output. The blocker is the
five layers below, none of which can be verified in isolation while the golden corpus must stay
byte-identical:

1. **RHI.** *Landed in RHI 1.5 (#471).* `BlendMode::DualSource` is in
   [`types.hpp`](../../include/tessera/rendering/rhi/types.hpp),
   `Device::supports_dual_source_blend()` is the capability that gates it, Vulkan enables
   `dualSrcBlend` only when the physical device reports it and implements the SRC1 factors, WebGPU
   answers false and rejects the pipeline request instead of mapping it, the null backend records the
   mode on the created pipeline, and `batched2d_lcd` is a compiled Vulkan-only manifest entry no
   frame path creates ([`gpu/rhi.md`](gpu/rhi.md) section 4.1). The remaining text below is the
   analysis that ordered the work, kept because steps 2 and 3 still depend on it.
   Before 1.5: `BlendMode` had two values (`include/tessera/rendering/rhi/types.hpp:192`); the Vulkan
   blend state implemented Alpha only
   (`src/rendering/backend/vulkan/src/vulkan_pipeline.cpp:111-122`); device creation enabled
   `dynamicRendering` and `timelineSemaphore` and nothing else
   (`src/rendering/backend/vulkan/src/vulkan_instance.cpp:298-322`), so `dualSrcBlend` needed both a
   feature probe and a capability the window can consult; and the WebGPU mapping silently turned any
   non-Alpha mode into blending *off* (`src/rendering/backend/webgpu/src/webgpu_mapping.cpp:128`),
   which for dual-source coverage would write raw coverage instead of falling back - it must reject
   the mode, not map it.
2. **Batch identity.** The renderer binds one pipeline per pass
   (`src/rendering/gpu/batched2d_renderer.cpp:135`) and `Batch` has no pipeline field
   (`include/tessera/rendering/gpu/batched2d_renderer.hpp:238`). A per-run pipeline adds a fourth
   axis to the split rule that already holds texture, scissor and folded clip state, and to both
   replay loops in `flush()`.
3. **Atlas identity.** *Landed in slice 2 (#471).* `GlyphAtlasConfig` now carries `aa_mode`
   alongside `hinting` (`include/tessera/ui/text/glyph_atlas.hpp`), so one atlas is one (mode,
   hinting) pair; `ScaleGlyphAtlas::set_aa_mode()` mirrors `set_hinting()` and releases the whole
   set; the rasterizer takes a `GlyphAaMode` and asks FreeType for `FT_RENDER_MODE_LCD`
   (`src/ui/text/glyph_raster.cpp`); the page upload writes the three subpixel coverages into RGB
   with the per-pixel maximum in alpha, so the page format and the texture upload are unchanged;
   and `GlyphAtlasEntry::channels` (1 or 3) is what the renderer divides the quad width by. Before
   slice 2: `GlyphAtlasConfig` carried `hinting` only, the page upload wrote coverage into alpha,
   and the rasterizer hard-coded `FT_RENDER_MODE_NORMAL`. The atlas identity space therefore
   doubles (mode x ladder step), but the *live* set does not: a mode change releases every atlas the
   way a hinting change does, so the set is still one atlas per ladder step, and a mode switch costs
   one re-rasterization exactly like a DPI change. What LCD does cost per frame is bytes: three
   coverage samples per pixel means a page holds about a third as many glyphs, so the unchanged page
   budget (`max_pages`, same LRU) fills up sooner.
4. **Eligibility has no home in the paint seam.** `TextDraw`
   (`include/tessera/ui/paint/paint_sink.hpp:19`) carries no backdrop, transform or opacity fact;
   `PaintContext` (`include/tessera/ui/paint/paint_pass.hpp:95`) has no "the window is opaque" fact;
   and the per-node paint cache replays recorded `DrawText` ops
   (`src/ui/paint/paint_cache.cpp:126-134`) from a signature
   (`src/ui/paint/paint_pass.cpp:324`) that would not carry the new fact, so a cached run could
   claim LCD over a backdrop that changed since it was recorded. That is four coordinated edits to a
   public struct, the context, the cache signature and both replay paths.
5. **Presentation and determinism.** The window owns the pipeline
   (`src/app/window_recovery.cpp:113`), so the LCD pipeline must be created only when the resolved
   policy permits it and the device reports dual-source support, and handed to the renderer, the
   text renderer and the sink through all three composition roots (window, frame capture, designer
   preview). Goldens stay pinned to grayscale (section 7), so the only evidence for the feature is a
   lavapipe/real-GPU probe with an explicit tolerance.

Landing order that keeps each step verifiable on its own (each is a slice, not a commit inside this
one): (1) the RHI primitive only - `DualSource`, the device feature plus capability, the Vulkan
factors, WebGPU rejection, and the Vulkan-only `batched2d_lcd` entry compiled by `tessera_add_spv_c`,
with a pipeline-creation test; (2) the rasterizer and atlas mode - `FT_RENDER_MODE_LCD`, the mode in
the atlas identity, the RGB upload and the 3x-width quad; (3) the renderer pipeline axis, the pure
eligibility predicate, the paint-seam fact and `kAutoAllowsLcd`. Nothing in step 1 changes a frame,
which is what makes it reviewable and revertible. Step 1 landed as RHI 1.5 (#471): the
pipeline-creation test is `tests/gpu/rhi/dual_source_blend_tests.cpp`, and no renderer, window, paint
or text file was touched, so the golden corpus is unchanged.

Step 2 landed as the rasterizer and atlas half of #471. `GlyphAaMode` (`Grayscale` / `Lcd`) is a
parameter of `GlyphRasterizer::rasterize_glyph` and a field of `GlyphAtlasConfig`, so the mode is
atlas identity exactly like hinting; `ScaleGlyphAtlas::set_aa_mode()` releases the set for the same
reason `set_hinting()` does. `TextRenderer::set_aa_policy` maps `TextAaMode::LcdWhenEligible` onto
`GlyphAaMode::Lcd` and `draw()` takes the quad width from `entry.width / entry.channels`. Three
things are deliberately *not* in step 2, and the code says so where it matters:

- **The eligibility predicate and the pipeline axis are step 3.** `LcdWhenEligible` is the
  window-level permission, so the atlas rasterizes LCD for the whole window; nothing yet decides
  per run whether a run may *composite* LCD. Until step 3 an LCD atlas is drawn through the
  grayscale pipeline, which is why the upload also stores the per-pixel maximum in alpha: the run
  renders as grayscale rather than disappearing. That is a correctness fallback, not the feature.
- **FreeType's LCD renderer is available on the pinned build** (VER-2-13-3, built without
  `FT_CONFIG_OPTION_SUBPIXEL_RENDERING`): `FT_RENDER_MODE_LCD` produces `FT_PIXEL_MODE_LCD`, three
  interleaved coverage bytes per pixel, and the three channels genuinely differ across a stem
  (`tests/unit/scale/text_lcd_raster_tests.cpp` asserts a spread above 32). Without
  `FT_CONFIG_OPTION_SUBPIXEL_RENDERING` there is no LCD filter, so the three samples are the raw
  shifted-outline coverages - deterministic, and the reason no patent-encumbered filter is needed.
- **A glyph FreeType cannot render as LCD falls back to grayscale per glyph.** A bitmap-strike
  glyph is not an outline, and a color glyph is composited into a BGRA bitmap before any renderer
  runs, so neither can produce LCD coverage; the rasterizer detects the first from the glyph format
  and the second from the resulting pixel mode, and reports the mode it actually produced. The atlas
  entry then carries `channels == 1` for that glyph inside an LCD page, so the renderer's quad width
  stays right. A run therefore never loses a glyph because the display asked for subpixel coverage,
  and the fallback is repeated for a glyph with no outline at all (whitespace keeps its advance and
  has no bitmap in either mode).

The golden corpus is unchanged: `GlyphAaMode::Grayscale` is the default of both the rasterizer
parameter and the atlas config, `golden_aa_policy()` pins `TextAaMode::Grayscale`, and the grayscale
upload path and page background are byte-identical to before.

### 3.5 Contrast and gamma (T5)

Coverage blends in sRGB-encoded space (D2), so light-on-dark text looks thin and dark-on-light
heavy. **Adopted as a D2 amendment** (section 8 decision 2): the sampled coverage of each run is
multiplied by a boost taken from a 256-entry `constexpr` table indexed by the text color's encoded
lightness (a Rec. 601 proxy, quantized to a byte). The table is flat at 1.0 at and below mid
lightness (0.5, index 128) and rises linearly to exactly 1.4 at white, so dark-on-light text is
untouched and a white run is compensated by at most 1.4. The lookup runs once per run, not per glyph
and not in a shader branch.

Why a table rather than the formula: the curve then has exactly 256 possible values, the frame path
costs one index and one load, and a unit test pins the shape (monotonic, endpoints, no boost below
mid, 1.4 at white). The color-dependent step cannot live in the atlas upload path as first sketched:
an atlas page stores colorless coverage shared by every color, so baking a boost into it would need
one atlas per run color. The amendment therefore touches only the per-run coverage multiplier;
D2's fills, images and encoded blend space are unchanged.

### 3.6 What stays

An SDF/MSDF atlas stays P1-optional (D6): it is a scaling tool, not a display-quality tool.
Platform rasterizers (DirectWrite/CoreText) stay rejected (ADR-006, determinism).

## 4. Everything that is not text

The same facts apply to the rest of the renderer; each row is independent of text.

| Component | Today | Display-aware change | Slice |
|---|---|---|---|
| SDF rounded rect and border | fixed `aa_radius` 0.5 local | edge half-width is `aa_radius` device px: the local-per-pixel factor comes from the derivative of the interpolated local coordinate (exact, unlike `fwidth(d)` at rounded corners); golden keeps radius 0 | S2 |
| Hairlines / 1 CSS px borders | width follows scale, not snapped | snap to whole device px, minimum 1; reuse `pixel_snap` (`src/ui/layout/pixel_snap.cpp`) at paint time | S3 |
| Rounded clip edge | hard pixel-center mask | coverage from the clip's SDF with the same `fwidth` width; clip stack multiplies coverage | S4 |
| Axis-aligned quads, fractional edges | can alias | snap at paint (S3); only transformed quads stay soft | S3 |
| Images / textured quads | nearest or linear by flag | filter by scale: nearest at aligned 1:1, linear otherwise, mips when downscaling; decode at display size x DPR (#14) | S8 (filter; mips blocked on the RHI) |
| Vector paths | no rasterizer yet | rasterize at device scale, cache per ladder step; strokes clamp to 1 device px minimum | with ThorVG (D1) |
| Offscreen layers / glass | layer at 1x unless told | layer targets allocated at the window's render scale and keyed by it | with layers |
| MSAA 4x | per-target opt-in | policy picks it only for non-SDF, non-text geometry and scale < 2.0; never default | S8 (rule and presentation hook) |
| FXAA / TAA / post AA | out of V1 | unchanged, out | - |

Rule: SDF AA plus pixel snapping carries the 2D UI; MSAA is the exception, not the answer. Clip AA
(S4) is display-independent in cause but shares the `fwidth` helper, so it ships with S2.

## 5. Configuration

`renderer.textAa`: `auto` (default, the policy above) | `grayscale` | `subpixel`.
`renderer.aaMode` / `aaRadius` / `msaaSamples` as in `antialiasing.md` section 3. Presets only, no
per-node radius (`open-questions.md`, antialiasing#5). The power policy
([ADR-019](../../decisions/ADR-019-power-aware-runtime.md)) may force `grayscale` and `msaa=1`. A
user setting never overrides golden mode.

The `renderer.textAa` key is not wired to a window surface yet, so the resolver sees `auto`
(grayscale). The explicit switch is `WindowOptions::display_aware_aa`, default **on**. Turning the
policy on is what changes default text hinting from the font's own hints to light at 1.0-1.5x, so the
switch is the deliberate opt-out that restores the pre-policy behavior. Golden mode overrides it: a
renderer configured with `golden_mode`, or with AA disabled, always resolves the golden policy
regardless of the switch and of the display.

## 6. Slices

| Slice | Content | Tests |
|---|---|---|
| S1 | `AaPolicy` resolver + HAL `PixelGeometry` (Unknown default; Win32 fills it from ClearType in S7, X11/Wayland later) | policy table unit test with fake displays |
| S2 | `fwidth` edge in `batched2d.frag` for SDF and border | edge-width probe at 0.5x/1x/2x/45 deg; golden unchanged |
| S3 | paint-time snap of hairlines and borders | 1.25/1.5 scale fixtures, 1-device-px assertion |
| S4 | rounded-clip coverage AA | clip corner has intermediate alpha; nothing written outside the clip |
| S5 | text T1 + T2 (device raster size, hinting by scale) | metrics unchanged across scales; atlas key test |
| S6 | text T3 (subpixel positioning) | golden mode unaffected; bounded atlas growth |
| S7 | text T4 (LCD: RHI primitive, rasterizer + atlas mode landed; eligibility + pipeline later) + T5 (contrast LUT, adopted as a D2 amendment and landed) | LCD only on eligible runs; fallback test; contrast LUT table test |
| S8 | image filter by scale, MSAA policy hook | filter selection table test |

Window integration (landed): `AppWindow` resolves one `AaPolicy` from its `hal::WindowMetrics`
(dpi scale, pixel geometry, transparency, orientation) and the renderer configuration (`textAa`, AA
enabled, MSAA samples, the power preset's reduced quality, golden mode) at creation and on every
metrics change - never on the frame path - stores it as plain data, and applies it to
`TextStack::renderer` with `TextRenderer::set_aa_policy` between frames. The timing is the one
`set_render_scale()` already needs: a hinting change releases every atlas page, so applying it
mid-frame would leave batched quads with dead textures. The window also sets
`PaintContext::device_scale` to the window's device scale, which makes S3 live.

The frame transform is now live too: at frame start the window composes
`ui::scale::content_scale_for(device_scale, adaptive_x, adaptive_y)` - the device scale times the
adaptive render-scale factor - and sets it with `Batched2DRenderer::set_content_scale`. Scene
geometry is authored in CSS px, so that one factor is what makes T1's CSS-sized glyph quads cover
exactly their device-pixel bitmaps (1:1 on a ladder device scale) and puts the snapped borders on
the device grid. The same `metrics.dpi_scale` value feeds snapping, the policy's input fact and the
transform, so they cannot drift; the policy still quantizes it to a ladder step for glyph
rasterization by design (section 3.1). On an off-ladder display (e.g. 1.1667) text is therefore
rasterized at the nearest step and resampled by at most half a step; scaling the geometry by the
ladder step instead would overflow the physical target. At 1.0 both factors are exactly 1.0 and
`set_content_scale` is never called, so the 1.0 frame - every golden - stays byte-identical.

The clip path needs no separate scaling: `push_clip` scales the paint-space clip before the CPU
scissor is derived from it (`ClipStateTable::push` -> `enclosing_scissor`), so the scissor fast path
and the folded SDF uniform test the same target-space rectangle. `set_scissor` itself is a
target-pixel seam: the frame scissor is the target extent and damage regions are already device
pixels. Still missing, all later PRs: X11/Wayland `PixelGeometry` sources (they report `Unknown`),
LCD compositing (T4's eligibility predicate and pipeline axis, section 3.4.1 step 3 - the
rasterizer and atlas half landed), and the mip-chain downscale case below.

S8 image filter (landed): RHI sampler state is fixed at texture creation, so the filter is part of
`ImageTextureKey` and `Batched2DSink::draw_image` picks it per draw with the pure
`ui::paint::image_filter_for_draw(rect_device, raster_extent, offset_device)`: `Nearest` when the
drawn device extent equals the uploaded raster extent within half a device pixel on both axes and
the device-space origin is a whole pixel, `Linear` otherwise. Because an image is decoded at display
size x DPR (#14), the raster extent already follows the device scale, so an integer CSS box whose
device origin is whole stays 1:1 (and nearest) even on a fractional ladder step; the linear path is
what covers fractional device origins, explicit raster extents and minification. Keeping two
variants of one image doubles its cache entries (the limits are per entry), which is the documented
cost of a per-draw choice.

**The design's third case, mips when downscaling, is blocked on the RHI, not on the policy.**
`rhi::TextureDesc` has no mip-level count and no backend generates a chain, so a magnified texture
has no lower level to sample; this slice deliberately does not fake it. A downscaled draw therefore
resolves `Linear` at level 0, and the filter decision covers only the aligned-1:1 and scale cases
until the RHI grows mip levels (then the key gains the level count and minification selects a
level). No doc or test claims mip sampling works.

S8 MSAA presentation hook (landed): the resolver's `msaa_samples` is consumed per presentation
generation. `window_aa_policy` is resolved before the pipeline is created, and
`presentation_sample_count(policy, golden, device->supports_sample_count(format, 4))` gates it: the
scene pipeline, `Batched2DConfig::sample_count` and the per-slot offscreen frame target all get the
same value, so the renderer's sample-count check can never see a mismatch. At 1 (the default) no
offscreen target or present pipeline is created and the frame records exactly the pre-S8 commands.
At 4 the scene renders into a 4x offscreen target and the frame is presented by sampling that
target's resolved image into the swapchain image (RHI 1.3 sampled target) with one full-target quad
on a single-sample, no-blend pipeline - `blit_target` rejects multisampled sources, so this GPU-only
path replaces the POC's demo CPU readback bridge. The sample count is fixed for a presentation
generation (a metrics change that crosses `kMsaaMaxScale` takes effect at the next generation,
exactly like the swapchain format), and a 4x frame target cannot be read by the glass backdrop copy,
so glass reports its draws unsupported for that generation (the documented bounded fallback).

S1 details: `Auto` text mode resolves to grayscale until LCD text lands (`kAutoAllowsLcd`);
`WindowMetrics::pixel_geometry` and `DisplayInfo::pixel_geometry` default to `Unknown`, and Win32
fills both from `hal::clear_type_geometry` (section 2). The resolver is pure and covered by
`aa_policy`.

S7 first half (landed): `hal::clear_type_geometry(font_smoothing, orientation, type)` is the pure
mapping the unit test exercises without Windows APIs; `query_win32_pixel_geometry()` reads the three
`SystemParametersInfoW` values, maps anything it does not understand to `Unknown`, and both
`fill_monitor` (the `DisplayInfo` list) and `fill_win32_display_capabilities` (the per-event
`WindowMetrics`) call it, so the topology and the window snapshot can never disagree. The mapping
distinguishes "the OS says subpixel AA is off" (`None`) from "this build could not ask" (`Unknown`)
instead of collapsing both into a guess.

Order: S1 first (everything reads it), then S2 + S4 together, S3, S5, S6. T5 landed with the D2
amendment; S7's ClearType fact landed separately from its LCD half (section 3.4).

## 7. Determinism and goldens

Golden mode pins `AaPolicy` to `{ scale 1.0, grayscale, fixed hinting, no subpixel positioning,
edge radius 0, clip AA off, msaa 1 }` and ignores all facts and user settings. At scale 1.0 the text
path is byte-identical to the pre-T1 behavior: the device pixel size equals the CSS pixel size and
the quad placement is the previous whole-pixel snap. The contrast LUT is a constant, not a fact read,
so a golden run's colors choose the same boost on every machine. The S8 filter table resolves
`Nearest` for the 1:1 integer geometry every golden uses, and the CPU reference sink always blits
nearest, so image goldens keep their sampling; a golden run also pins MSAA to one sample, so the 4x
present path never records a command in a deterministic corpus. New behavior gets
its own corpus only where it can be deterministic: CPU goldens stay tolerance 0
(`antialiasing.md` section 2.2); LCD and `fwidth` edges are covered by lavapipe probes with an
explicit tolerance, never by real-GPU goldens.

## 8. Decisions needed

1. Ship LCD text in V1, or grayscale only with T1-T3? **Decided: LCD ships in V1 in a later PR.**
   Until then `Auto` resolves to grayscale (`kAutoAllowsLcd == false`).
2. Adopt the text contrast LUT (T5), and does it count as a D2 amendment? **Resolved: adopted as a
   D2 amendment and implemented** (section 3.5; the amendment is recorded with D2 in
   [`rendering-production-plan.md`](rendering-production-plan.md)). Only the per-run text coverage
   multiplier is new; D2's encoded blend space and its fill/image rules are unchanged.
3. Windows: take `PixelGeometry` from the OS ClearType setting, or detect per display only?
   **Decided: detect per display, Windows first - and the honest Windows answer is the user's
   ClearType setting, because no per-monitor query exists** (section 2). Both
   `DisplayInfo::pixel_geometry` and `WindowMetrics::pixel_geometry` are filled from it, and the
   desktop-wide limitation is documented rather than hidden behind a fabricated per-panel value.
4. Clip AA (S4) in V1, or accept hard rounded clips until MSAA? (Recommendation: S4 in V1.)
