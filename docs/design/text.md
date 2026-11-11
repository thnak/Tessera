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
rasterization are not implemented. ICU itself is not integrated: there are no ICU call sites
(HarfBuzz is built with `HB_HAVE_ICU=OFF`) and no ICU data package; the measured reduced-data
cost, the locale set, and the GO decision are in section 13.
[ADR-006](../../decisions/ADR-006-text-stack.md) (text stack) selects
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
- Emoji and color fonts: issue #12 decided on one bundled COLRv0/CBDT emoji face as the last entry
  of the stack, drawn through an RGBA atlas page next to the coverage pages, with no COLRv1
  gradients yet. That supersedes ADR-006 item 6 ("not in V1") once slices 12b and 12c land; fallback
  must stay deterministic either way.
- Fallback decisions are cached per `(font stack, codepoint sequence)`.
- A missing glyph renders a deterministic tofu/placeholder and emits a diagnostic; it never
  silently changes the font stack across runs.

Implemented (slice 12a, `ui/text/fallback_shaper.*`, test `fallback_shaper`): `FallbackShaper` takes
the ordered stack and cuts the text into clusters (a base plus combining marks, variation
selectors, emoji modifiers, ZWJ and tag sequences, regional-indicator pairs). Each cluster goes to
the first face covering all of it (else the first face drawing its base character, else face 0 and
`.notdef`), adjacent clusters of one face are shaped together, and `Glyph::face` records the stack
index. `ShapedRun::missing_glyphs` counts the `.notdef` glyphs for the diagnostic. Sub-runs are
shaped with the direction of the whole text and joined in visual order. The cluster-to-face
decision cache is bounded at 4096 entries and restarts when full. Plan: 12b makes the glyph atlas
and `TextRenderer` face-aware (a rasterizer per face, atlas key `(face, glyph, size)`), 12c adds
FreeType color loading (`FT_LOAD_COLOR`), RGBA pages and the bundled emoji face with its
`THIRD_PARTY_NOTICES.md` and `dependencies.md` entries.

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
- Not implemented: skyline packing, subpixel positioning, SDF/MSDF, multi-face rendering (shaping fallback exists, see section 4), async
  uploads, and per-glyph (rather than per-page) eviction.
- Hinting/AA settings come from the pinned FreeType build; no system FreeType configuration is
  read.
- Atlas page format is independent of the ICU data question in section 12: subsetting changes
  only which locale bundles ship, never page packing, the RGBA8 page format, or eviction. The
  measured data cost and GO decision are in section 13. The UAX #14 wrap (section 6) is likewise
  atlas-neutral: it changes line boxes only, never glyph packing, page order, or eviction.

## 6. Line breaking and alignment

Implemented greedy wrap (task-106; `white-space` modes task-110). `ui/text/text_wrap.cpp` lays out
one paragraph at a time (a hard newline splits paragraphs) over **UAX #14 line-break segments** from
`ui/text/text_breaks.*`: each segment is the smallest line unit, and a segment wider than the
constraint is split at the last measured code-point prefix that fits and then at the last legal break
inside that prefix (`line_boundaries` again: hyphen, ZWSP, CJK). One segmentation pass per paragraph
feeds the whole wrap, so no line ever needs a second one.

The break engine is the ICU data switch, not a separate option: the builtin reduced rules when
`TESSERA_ICU_REDUCED_DATA=OFF` (the default and every CI job), ICU `ubrk_*` iterators when `ON`.
`layout_text_with_breaks` (private `src/ui/text/text_wrap.hpp`) takes the engine explicitly, which
is how the parity test drives both through the whole wrap path rather than only the boundary sets.

### 6.1 `white-space` modes (task-110)

`TextStyle::white_space` (`WhiteSpace` in `ui/text/text_measure.hpp`) is the measured wrap contract.
`text_style_from()` maps the computed `white-space` keyword to it, and the paint pass reads only that
field, so a stylesheet value reaches the wrap without a second parse.

| Mode | CSS keywords | Interior run | Line edges | Wraps |
|---|---|---|---|---|
| `Normal` | `normal`, `pre-line`, unknown | collapses to one space advance | trimmed | yes |
| `NoWrap` | `nowrap`, `pre` | literal | literal (whole text, one line) | no |
| `PreWrap` | `pre-wrap` | literal | trimmed | yes |
| `BreakSpaces` | `break-spaces` | literal; every preserved space is its own unit | preserved | yes |

- **Collapsing** (`normal`) measures an interior separator run as the advance of one space
  (`measure(" ")`), not the run: `foo   bar` is 56 px at 16 px stub metrics instead of 72. The
  *painted slice* still spans the source bytes, because a `TextLine` is a source range: the measured
  width is the collapsed one, and a real collapse stage is the remaining contract below.
- **Trimming** is the task-106 LB18 rule: a whitespace run belongs to the segment before it, so a
  break always sits *after* the spaces and the wrap trims both edges of every segment. A trailing
  space never advances a line and never reaches the painted slice; a leading space never starts one.
- **`break-spaces`** preserves every run and gives each ASCII collapsible whitespace character its
  own line unit, so a break opportunity exists after every preserved space and a line may start,
  end, or consist only of spaces. A non-space run stays one unit unless it is wider than the
  constraint, where the over-wide split uses the last legal UAX #14 break inside the prefix. A unit
  that alone exceeds the constraint (a space narrower than the line, for example) owns its line and
  overflows it; that is the only way a line exceeds `max_width`.
- **`nowrap` keeps the pre-task-110 path**: one line with separators measured literally, so it
  preserves runs instead of collapsing them the way CSS `nowrap` does. The deviation is deliberate,
  because that path is what every existing no-wrap paint fixture and golden pins. `pre` maps here
  (it also preserves and does not wrap); collapsing `nowrap` needs the same text-transform stage
  `normal` needs for painting, so both are deferred together.
- `wrap = false`, a mode that does not wrap, and a non-finite width all take the single-line path.

Fixture deltas (task-110). Every fixture whose corpus is single-spaced ASCII is unchanged: with one
space between words "one collapsed space" and "one segment gap" are the same number, so the default
(`Normal`) keeps the task-106 line boxes, the CPU/GPU text goldens, and two consecutive runs
byte-identical. Only the deliberately re-baselined multi-space fixtures move (stub metrics, 16 px
font = 8 px per code point):

| Fixture | task-106 | task-110 default (`normal`) |
|---|---|---|
| `foo   bar` at 200 px | width 72 | width 56 |
| `alpha  beta` paint line | width 88 | width 80 |
| `foo   bar` at 56 px | 2 lines (`foo` / `bar`) | 1 line, width 56 |
| `a\t\tb` at 200 px | width 32 | width 24 |
| `a\t\tb` at 24 px | 2 lines (`a` / `b`) | 1 line, width 24 |
| `hello world   ` at 200 px | width 88 | unchanged |
| `   hello` at 200 px | width 40 | unchanged |
| em dash `aaaa—bbbb` at 56 px, builtin | `[0,9)` / `[9,11)` | unchanged |
| em dash same, ICU | `[0,7)` / `[7,11)` | unchanged |

The mode matrix is pinned in `tests/unit/text_layout_tests.cpp` (interior runs, leading/trailing,
tabs, blank lines, CJK + spaces, em dash, determinism) and `tests/unit/paint_text_wrap_tests.cpp`
(collapse and preservation reaching paint, a line of preserved spaces, break-spaces overflow).
`text_breaks_parity` runs the **same corpus per mode through both engines** and requires identical
line boxes, plus the em-dash BA difference for every narrowing mode.

Remaining contract, not implemented in this slice:

- A text-transform/collapse stage for painting: `normal` measures a collapsed run but paints the
  source bytes of the line's range, so an emitted slice can be wider than its line box until that
  exists. `nowrap` collapsing and `pre-line` newline handling (`pre-line` maps to `normal` because
  the paragraph split already honors hard newlines) land with it.
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
  **Resolved (task-94): subset it, and ship per-locale packs.** Measured on the pinned ICU
  76.1 package: 30.37 MiB full, 13.63 MiB with every non-locale item kept (the Unicode
  floor), 8.59 MiB for 21 locales once converters/transliteration/spellout/collation are
  dropped (6.54 MiB English-only, ~103 KiB per extra locale). The option stays opt-in
  (`TESSERA_ICU_REDUCED_DATA=OFF`) because ICU has no call sites yet. Failure modes,
  locale set, CI cost, and the GO conditions: section 13.
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

Bidi layout (task-71): SheenBidi v3.0.0 (Apache-2.0, pinned archive `86c560…`) resolves the
paragraph direction (UBA P2/P3 first-strong) and per-line visual runs after the unchanged greedy
wrap. `TextLine` keeps its logical byte range and gains optional `visual_runs` (begin/end/level, in
visual order; empty means logical order is visual order, so pure LTR layout/paint stays
byte-identical); `TextLayout` records the paragraph `direction`. `bidi_offset_x` maps a logical
caret/selection offset to a visual x with the documented right-biased run-join affinity (at a join
the offset belongs to the run whose logical range ends there; RTL runs map from their right edge).
Paint consumption of the visual runs and wiring the edit controller's caret to it is the next
additive slice. The deterministic fixture font has no Hebrew/Arabic glyphs, so bidi tests are
structural-only for now; pixel proof needs an approved bidi-capable OFL test font (deferred).

Bidi paint (task-74): paint now consumes `TextLine::visual_runs` and emits each line's runs in visual
order using the shaped advances; a line with empty runs is byte-identical to the pre-bidi emission.
Caret/selection use `bidi_offset_x` (right-biased join affinity); selection paints one rect per
visual run so a selection crossing a direction change is the union of run intervals. The test-only
OFL-1.1 Hebrew fixture is `tests/fixtures/fonts/NotoSansHebrew-Regular.ttf` (not linked into the
app). Click-to-position inside RTL/mixed runs stays a documented deferral; no CPU golden yet (the
CPU text scene has no bidi payload).

### 12.1 Point-to-caret hit testing (task-79)

`hit_test_point` maps a content-box-local point to `{byte_offset, CaretAffinity}`. Lines are chosen
by y (above/below clamp to the first/last line), then x resolves within the line's visual runs: LTR
runs pick the nearest cluster boundary by shaped advance; RTL runs map from the run's visual right
edge back to logical clusters; a point exactly on a run join uses the right-biased affinity (the run
whose logical range ends there, `Upstream`). The result always snaps to a grapheme-cluster boundary
(existing `next_cluster` rules), never a code-point interior. Ties choose `Downstream` in LTR. The
caller subtracts any centered/end alignment before calling; the metrics advance source is a
parameter so edit hit testing matches the painted font. `EditController::place_caret_at` collapses
the selection and moves the caret; `extend_selection_to` keeps the anchor (Shift+click). Read-only
fields allow caret/selection moves. Deferrals: drag selection and selection handles, word/line
double/triple-click segmentation, vertical text, sub-pixel affinity, and selection while composing.

## 13. ICU reduced static data (measured, task-94)

Task-94 measured whether a reduced ICU data package is affordable and how a subset fails; it
closes the section 12 open question. Nothing is on by default: the data step sits behind
`TESSERA_ICU_REDUCED_DATA=OFF`, so the default build and CI are unchanged. Method, full
per-tier numbers, the failure table, and the follow-up slice are in
[`icu-data.md`](icu-data.md).

Facts on the spike commit: ICU was neither vendored nor fetched (no `FetchContent`, no
`find_package`, no version pin) and had zero call sites (HarfBuzz built with `HB_HAVE_ICU=OFF`);
segmentation/line breaking used bespoke `ui/text/edit_grapheme.*` helpers and greedy wrap,
`services/i18n` is spec-only, and collation is optional and unused. ADR-012 rule 5 therefore had
nothing to satisfy then.

Measured on pinned ICU 76.1 (`icudt76l.dat`, 31,851,856 B) trimmed with the pinned `icupkg`:

| Tier | Items | Blob bytes | MiB | Of full | Link delta |
|---|---|---|---|---|---|
| Full upstream package (924 locales) | 4,136 | 31,851,856 | 30.37 | 100% | +31,839,232 |
| Base: `root` only, all non-locale data | 269 | 13,633,072 | 13.00 | 42.8% | - |
| Base + `en` | 279 | 13,737,088 | 13.10 | 43.1% | - |
| Recommended (22 ids, 23 after closure) | 447 | 17,293,904 | 16.49 | 54.3% | +17,281,024 |
| Recommended, converters dropped | 257 | 12,136,048 | 11.57 | 38.1% | - |
| Recommended aggressive (+ translit/rbnf/coll) | 213 | 8,593,024 | 8.19 | 27.0% | +8,580,096 |
| Base + `en`, aggressive | 82 | 6,539,328 | 6.24 | 20.5% | +6,526,464 |

The bulk is not locale data: 42.8% of the package is locale-independent Unicode data (a
13.63 MiB floor), 924 locale packs are 57% (~169 KiB each, ~103 KiB once collation is dropped),
and 5.16 MiB of the recommended tier is legacy charset converters a UTF-8-only framework does not
need. Build cost is negligible: +0.20 s warm configure and 1.0 s for the target; a cold lane
configure is 45.7 s either way. The linked binary delta equals the blob.

Failure modes (probe `third_party/icu/experiment/icu_subset_probe.c`, absent locale `zz_ZZ`
versus `ja_JP`): break iteration, collation, number and date formatting all **fall back to root**
with `U_USING_DEFAULT_WARNING`; `udat_open` returns **`U_ZERO_ERROR` while emitting root
patterns**, so a missing pack silently renders wrong dates; Unicode core (normalization, case
mapping, character data) always works because that data is never trimmed. A trimmed locale and a
never-present one are the same code path for ICU, which only sees the package ToC.

**GO**, with three conditions: (1) ship locale packs, not the full package — 8.59 MB for 21
languages, 6.54 MB English-only, ~103 KiB per extra locale; (2) keep ICU opt-in until the text
stack uses it, so CI stays unchanged; (3) ship a kept-locale manifest and make a missing locale a
`LocaleResolver` diagnostic instead of a silent fallback. Recommended set
(`third_party/icu/locales.txt`): `root, en, en_GB, de, fr, es, it, pt, pt_BR, nl, pl, ru, tr, ar,
he, hi, th, vi, ja, ko, zh, zh_Hant`. CI cost: zero as landed; a release job that enables the
step pays +0.20 s configure and +1.0 s build.

### 13.1 Implementation (task-101)

The follow-up slice landed; all three conditions are met and the option is still OFF:

- `third_party/icu` compiles pinned ICU 76.1 into static `tessera_icu_common` / `tessera_icu_i18n`
  archives and links the reduced blob (447 items, 22 locale bundles, 17,293,904 B) as
  `tessera_icu_data`; `tessera_icu` is the module-facing INTERFACE target.
- `ui/text/icu_runtime.*` installs the blob with `udata_setCommonData`, asserts the pinned
  version, and proves the package is readable by opening the root bundle.
- `ui/text/text_breaks.*` owns break opportunities: `ubrk_*` word (UAX #29) and line (UAX #14)
  iterators, with the previous reduced rules kept as the ICU-free backend and the parity baseline.
  Word navigation and the greedy wrap go through it: `ui/text/text_wrap.*` places one UAX #14
  segment per line unit (section 6), so the whole wrap path is UAX #14, not only the over-wide-run
  split of the old word loop.
- `ui/text/locale_resolver.*` validates a request against the generated `locales.json` manifest
  and emits `icu.locale.fallback` / `icu.locale.missing`, so a trimmed locale can no longer format
  root data silently.

Parity matrix and every intentional difference (word boundaries, line opportunities, the full wrap
path with the builtin engine as baseline, and the per-`white-space`-mode matrix of section 6.1):
`tests/unit/icu/text_breaks_parity_tests.cpp`. Build shape, measured cost, deviations, and the
exact option flip: [`icu-data.md`](icu-data.md) section 8.