# Text: shaping, bidi, line breaking, and glyph atlas

Status: design for Big Step B2. `src/ui/text/` is a placeholder today. ADR-006 selects
FreeType + HarfBuzz + SheenBidi + ICU.

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
  -> line break (ICU / libunibreak) -> lines
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
- `direction`, `writing-mode` (parsed; vertical layout later)
- `font-feature-settings` (parsed; selected features later)

Units resolve through the style engine: `em`/`rem`/`%`/viewport units become pixels before
shaping. `line-height: normal` uses font metrics, not a hardcoded multiplier.

## 4. Font fallback

- A style has an ordered font stack.
- Missing glyphs fall through to the next face that covers the codepoint.
- Emoji/color fonts are a later slice; fallback must still be deterministic.
- Fallback decisions are cached per `(font stack, codepoint sequence)`.
- A missing glyph renders a deterministic tofu/placeholder and emits a diagnostic; it never
  silently changes the font stack across runs.

## 5. Glyph atlas

- Dynamic packing (shelf/skyline); deterministic insertion order in golden mode.
- Per-face/size caches; atlas pages are bounded.
- Eviction policy: LRU with a frame-age guard; never evict glyphs used by the current frame.
- Atlas updates are GPU texture uploads; they go through the resource/async path.
- Subpixel positioning is disabled in golden mode; normal mode may use it if deterministic
  enough for the target platform (documented per backend).
- Hinting/AA settings are fixed per build; do not depend on system FreeType configuration.

## 6. Line breaking and alignment

- ICU line breaking (or libunibreak fallback) for Unicode line/word opportunities.
- `white-space`: normal/nowrap/pre/pre-wrap/break-spaces (V1 subset documented).
- `text-overflow: ellipsis` needs measured ellipsis width; no mid-glyph clipping.
- `text-align`: start/end/left/right/center/justify (justify later).
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
  the HAL (`docs/design/input.md`).
- Composition text is rendered with an underline/selection style; the committed text replaces
  it atomically.
- Caret/selection are style/paint concerns; cluster boundaries come from HarfBuzz.
- Accessibility exposes text, selection, and caret position to the platform bridge.

## 10. Performance

- Shape only dirty text; cache shaped runs/layouts.
- Avoid per-frame atlas uploads; batch glyph updates.
- Use glyph quads with the existing `(scissor, texture)` batching key.
- Target: a 10k-glyph screen at 60 Hz with bounded cache growth.
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
- Whether to subset ICU data for binary size (ADR-012).
- Atlas format: R8 vs RGBA8, SDF vs raster for scaling.
