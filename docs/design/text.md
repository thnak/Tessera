# Text: shaping, bidi, line breaking, and glyph atlas

Status: design for Big Step B2 (runtime slice; see
[AGENTS.md Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step)). `src/ui/text/` now has
the measurement seam: `TextMeasurer`, `TextStyle`, `TextConstraints`, `TextMetrics`, and a
deterministic `StubTextMeasurer` (fixed advance, greedy word wrap) wired into Yoga through
`LayoutInput::text`. `FontMetricsProvider`/`StubFontMetricsProvider` supply ascent/descent/line-gap
for baselines and `line-height: normal`; `TextLayout` exposes per-line width/height/baseline from the
same wrapping pass used by measurement. `TextMeasureCache` adds a bounded LRU keyed by
text/style/constraints with hit/miss/eviction/invalidation stats. Font files now load through
FreeType (`ui/text/font_face.hpp`) and `FreeTypeFontMetricsProvider`
(`ui/text/freetype_metrics.hpp`) scales the face's design metrics to the requested pixel size; the
deterministic test face is `tests/fixtures/fonts/NotoSans-Regular.ttf` (OFL-1.1).
`HarfBuzzShaper` (`ui/text/harfbuzz_shaper.hpp`, behind the `ui/text/text_shaper.hpp` seam) turns
UTF-8 runs into glyph ids, byte clusters, and pixel advances, including kerning and standard
ligatures. `GlyphRasterizer` (`ui/text/glyph_raster.hpp`) renders hinted 8-bit coverage,
`GlyphAtlas` (`ui/text/glyph_atlas.hpp`) shelf-packs bounded RGBA8 pages with a frame-age LRU, and
`TextRenderer` (`ui/text/text_renderer.hpp`, target `tessera_text_render`) emits glyph quads through
`Batched2DRenderer`. Bidi run splitting, line breaking, font-stack fallback, and SDF/subpixel
rasterization are not implemented. [ADR-006](../../decisions/ADR-006-text-stack.md) (text stack) selects
FreeType + HarfBuzz + SheenBidi + ICU (licenses and module ownership in
[`dependencies.md`](../specs/dependencies.md)). Terms: IME = input method editor; HAL = hardware/OS
abstraction layer ([`hal.md`](hal.md)); golden = deterministic reference image; atlas = GPU texture
holding packed glyph bitmaps; SDF = signed distance field; tofu = the placeholder box drawn for a missing
glyph; V1 = first release (scope rule in [AGENTS.md Non-negotiables](../../AGENTS.md#non-negotiables) item
8). See also [`glossary.md`](../reference/glossary.md).

## 1. Stack

| Layer | Library | Responsibility |
|---|---|---|
| Font files | committed assets | deterministic faces; no system fonts in goldens |
| Font loading/raster | FreeType | face parsing, glyph rasterization, metrics |
| Shaping | HarfBuzz | scripts, ligatures, kerning, clusters, features |
| Bidi | SheenBidi | paragraph direction, runs, levels |
| Unicode | ICU | segmentation, normalization, line breaking, locale data |
| Atlas | Tessera | glyph packing, caching, GPU texture updates |
| Layout | Yoga callback | measure text runs; no rasterization |

## 2. Pipeline

```text
text + TextStyle + constraints
  -> itemize by script/language/direction (ICU/HarfBuzz)
  -> bidi paragraph (SheenBidi) -> visual runs
  -> shape each run (HarfBuzz) -> glyph ids + advances + clusters
  -> line break (ICU / libunibreak, the optional fallback listed in dependencies.md) -> lines
  -> glyph raster (FreeType) -> atlas entries
  -> quads -> batched 2D renderer
```

The result is a **shaped text layout**:

```cpp
struct ShapedRun {
    FontId font;
    float size;
    Direction direction;
    std::vector<Glyph> glyphs;
    std::vector<Cluster> clusters;
    float advance;
    float ascent;
    float descent;
};

struct TextLayout {
    std::vector<Line> lines;      // runs per line
    float width;
    float height;
    float baseline;
};
```

## 3. Styles

V1 text style properties:

- `font-family` (stack), `font-size`, `font-weight`, `font-style`
- `line-height`, `letter-spacing`, `word-spacing`
- `text-align`, `text-transform`, `white-space`, `text-overflow`
- `color`, `opacity`
- `direction`, `writing-mode` (parsed; vertical layout is not in V1, see section 12)
- `font-feature-settings` (parsed; which features are honored is not specified)

Units resolve through the style engine: `em`/`rem`/`%`/viewport units become pixels before
shaping. `line-height: normal` uses font metrics, not a hardcoded multiplier.

## 4. Font fallback

- A style has an ordered font stack.
- Missing glyphs fall through to the next face that covers the codepoint.
- Emoji/color fonts are not in V1 ([ADR-006](../../decisions/ADR-006-text-stack.md) item 6; no release is named for them); fallback must still be deterministic.
- Fallback decisions are cached per `(font stack, codepoint sequence)`.
- A missing glyph renders a deterministic tofu/placeholder and emits a diagnostic; it never
  silently changes the font stack across runs.

## 5. Glyph atlas (implemented subset)

- `GlyphRasterizer` rasterizes hinted `FT_PIXEL_MODE_GRAY` coverage at integer pixel sizes; a
  missing codepoint renders `.notdef` and keeps its advance.
- `GlyphAtlas` shelf-packs pages in deterministic insertion order. `GlyphAtlasConfig` bounds page
  size, page count, and padding. Pages are RGBA8 with white RGB and coverage alpha, so the batched
  2D shader needs no swizzle.
- Eviction is page-level LRU with a frame-age guard: a page touched in the current frame is never
  evicted. If every page was touched, the glyph is reported as dropped
  (`GlyphAtlasStats::dropped`) and only its advance is kept.
- `TextRenderer` owns one RHI texture per page, creates it from the page pixels, and updates it in
  place through `rhi::Texture::write` when the page generation changes. Each glyph becomes a
  textured quad with atlas UVs through `Batched2DRenderer`.
- Not implemented: skyline packing, subpixel positioning, SDF/MSDF, multi-face fallback, async
  uploads, and per-glyph (rather than per-page) eviction.
- Hinting/AA settings come from the pinned FreeType build; no system FreeType configuration is
  read.

## 6. Line breaking and alignment

- ICU line breaking (or the libunibreak fallback) for Unicode line/word opportunities.
- `white-space`: normal/nowrap/pre/pre-wrap/break-spaces (the V1 subset is not enumerated; see [`stylesheet-v1.md`](../specs/stylesheet-v1.md)).
- `text-overflow: ellipsis` needs measured ellipsis width; no mid-glyph clipping.
- `text-align`: start/end/left/right/center/justify (justify is not in V1, per ADR-006 item 6).
- Bidi: visual order per line; alignment uses the paragraph direction.
- Baseline: layout uses the first line's baseline for alignment; multi-line boxes document
  their baseline choice.

## 7. Caches

| Cache | Key | Invalidate on |
|---|---|---|
| Face | font file + face index | font asset change |
| Shaped run | font, size, features, script/direction, text | font/style/text change |
| Text layout | runs + constraints + break policy | style/font/constraints change |
| Glyph | face, size, glyph id, hinting | atlas/face change |
| Atlas page | packer generation | eviction/repack |

All caches are bounded and report hit/miss/eviction metrics.

## 8. Determinism

- Golden tests load fonts from committed files, never system fonts.
- Fixed pixel size, DPI, scale, and line-breaking data.
- No subpixel positioning in golden mode.
- Fixed hinting/AA; no environment-dependent FreeType settings.
- Identical text/style/constraints produce byte-identical shaped-run dumps.
- No wall clock, locale from the OS, or network fonts in goldens.

## 9. IME and editing

Full text editing (selection/caret/commands/undo/clipboard/IME/secure fields/nodes) is
normative in [`text-editing-v1.md`](../specs/text-editing-v1.md). This section is the text-stack
summary.

- IME composition is owned by the focused text input; the platform IME events are normalized by
  the HAL ([`input.md`](input.md)).
- Composition text is rendered with an underline/selection style; the committed text replaces
  it atomically.
- Caret/selection are style/paint concerns; cluster boundaries come from HarfBuzz.
- Accessibility exposes text, selection, and caret position to the platform bridge.

## 10. Performance

- Shape only dirty text; cache shaped runs/layouts.
- Avoid per-frame atlas uploads; batch glyph updates.
- Use glyph quads with the existing `(scissor, texture)` batching key (the B1 batched 2D renderer's draw-merge key; see [`renderer.md`](renderer.md)).
- Target: a 10k-glyph screen at 60 Hz with bounded cache growth (reference hardware is not specified).
- Track glyph cache hit rate, atlas pages, upload bytes, and shaping time.

## 11. Tests

- Shaping: LTR Latin, kerning/ligatures, Arabic, Devanagari, mixed scripts.
- Bidi: RTL/LTR paragraphs, numbers, isolates, mixed runs.
- Line breaking: spaces, hyphens, CJK, no-break spaces, long words, ellipsis.
- Fallback: missing glyph, emoji fallback, tofu diagnostics.
- Atlas: packing, eviction, frame-age guard, deterministic page order.
- Goldens: fixed fonts/DPI; text layout dumps + images.
- IME: composition/commit/cancel sequences.

## 12. Open questions

- Color emoji and COLR/CPAL support.
- Variable fonts and axis animation.
- Justification/hyphenation.
- Vertical writing modes.
- Whether to subset ICU data for binary size ([ADR-012](../../decisions/ADR-012-static-linking.md), static linking).
- Atlas format: R8 vs RGBA8, SDF vs raster for scaling.

Paint-time text runs (D9): `ui::PaintData` (`include/tessera/ui/scene/paint_data.hpp`) lets a scene
node carry a text run painted by the paint pass. It reuses this text stack end to end: `paint_scene`
emits `PaintSink::draw_text`, and `Batched2DSink` shapes through `TextShaper` and draws through
`TextRenderer`/`GlyphAtlas` (no second rasterizer). Paint-time text is single-line, top-aligned in
the node border box, and never affects layout; `paint-pass.md` section 10 has the payload/precedence
contract. Goldens: paint-text image goldens are GPU-only for now because the CPU canvas has no text
rasterizer.

Paint-time multi-line/wrapped text (task-39): `PaintData` adds `wrap` (NoWrap default | Wrap),
`max_lines` (0 = unlimited) and `text_align`; the paint pass reuses `text::layout_text` line boxes
(now with per-line byte ranges) with the content-box width and computed font-size/line-height, and
emits one `PaintSink::draw_text` per line with an absolute baseline. No layout pass is required and
PaintData never affects layout. `max_lines` truncates deterministically without ellipsis; bidi,
hyphenation, rich text, and selection remain deferred. See `paint-pass.md` section 10.1 for the
contract, batching notes, and Debug measurements of a 200-word paragraph.

CPU text reference (task-44): the software canvas now composites the same FreeType coverage
bitmaps as the GPU path (`CpuCanvas::blend_coverage`, integer straight-alpha source-over with
round-half-up) and the paint text path renders single-line and wrapped `PaintData` through
`batched2d --backend cpu --cpu-text`. `batched2d_cpu_text_golden` compares the CPU scene
(`dashboard_text_cpu.png`) at tolerance 0 on every platform, so text goldens no longer require a GPU
or lavapipe; GPU text goldens keep their 1-LSB/0.1% tolerance because GPU blending rounds in float
before UNORM conversion. The CPU path does not cover SDF/subpixel text, platform shaping beyond the
deterministic fixture, or rounded-clip AA. See `docs/guides/testing.md` section 4 for the
regeneration command and `antialiasing.md` section 2.2 for the blend rules.

Paint text-layout cache (task-46): `PaintData` wrapped-text line boxes can now be reused across
paints through a caller-owned bounded LRU (`ui::paint::TextLayoutCache`, passed via
`PaintContext::text_layout_cache`; null keeps the uncached path). Key = text, wrap width, font
size/weight/italic/letter-spacing/line-height, wrap mode, max_lines and metrics-source id; exact
byte accounting, default 64 entries / 256 KiB, LRU eviction, no allocation on hit, `invalidate()`
for font/metrics changes, and a disable-for-tests mode. Debug benchmark for the 200-word/300 px
paragraph (45 lines): fresh layout ~96.8 us, miss+insert ~98.1 us, cache hit ~0.75 us. Glyph-run
caching, shaping caches, incremental layout, and sharing the cache with the layout engine remain
deferred. See `paint-pass.md` section 10.1.1.

Paint metric sources (task-53): `PaintContext::text_metrics_source` selects how wrapped paint text
is measured. Null (default) uses `StubPaintTextMetrics` (deterministic stub metrics + the existing
`layout_text` path, so CPU/GPU goldens stay byte-identical); `FontPaintTextMetrics(face, id)` uses
FreeType metrics and the same HarfBuzz advances the glyph renderer draws, via
`text::layout_text_measured`. The source id is part of `TextLayoutCache` so stub and real entries
never mix; paint emits the source line boxes verbatim (slices, widths, heights, baselines, snapping
only at paint time), so real-font lines no longer overflow the content box. Debug measurements
(200-word/300 px): stub ~200.8 us, real ~13.2 ms per layout (per-word HarfBuzz in Debug), real cache
hit ~1.0 us; faster real measuring is deferred, as are bidi/ICU breaking, hyphenation, subpixel
positioning, emoji/fallback and a shaping cache.
