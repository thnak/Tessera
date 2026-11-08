# ADR-006: Text stack — FreeType, HarfBuzz, SheenBidi, ICU

- Status: Accepted
- Date: 2026-11-07
- Related: [ADR-003](ADR-003-thin-rhi-and-batched-2d-renderer.md) (thin render hardware interface and batched 2D renderer), [ADR-005](ADR-005-yoga-layout-adapter.md) (Yoga layout adapter), [ADR-009](ADR-009-deterministic-headless-golden-tests.md) (deterministic headless golden tests)

## Context

Text is a first-class part of a UI framework. It requires:

- font parsing and glyph rasterization
- shaping (ligatures, kerning, complex scripts, emoji)
- bidirectional text
- Unicode segmentation and normalization
- font fallback
- a glyph atlas and cache that behaves well under animation

Platform text APIs (DirectWrite, CoreText, AT-SPI text) are not cross-platform and are not
deterministic across machines, which conflicts with [golden](../docs/reference/glossary.md) testing ([ADR-009](ADR-009-deterministic-headless-golden-tests.md)).

## Decision

1. Use the following stack:
   - **FreeType**: font loading and glyph rasterization
   - **HarfBuzz**: shaping (all scripts, ligatures, kerning)
   - **SheenBidi**: bidirectional text
   - **ICU (International Components for Unicode)**: Unicode segmentation, normalization, and optional collation
2. Build a **glyph atlas** with dynamic packing and per-font-instance caches.
3. Cache **shaped runs** keyed by `(font, size, script/direction, text, features)`; invalidate
   on font changes.
4. Support **font fallback**: a font stack per style; missing glyphs fall through to the next
   font that covers the codepoint.
5. **Determinism rules**:
   - golden tests load fonts from committed files, never system fonts
   - no subpixel positioning in golden mode
   - fixed pixel size and DPI (dots per inch, the display scale)
   - disable hinting variations that differ across FreeType builds
6. [V1](../docs/reference/glossary.md) limits: no full emoji presentation stack (color fonts are not planned for V1; no later release is named), no
   justification/hyphenation. RTL and complex scripts are supported through HarfBuzz/SheenBidi.
7. Do not use platform text APIs for production rendering. They may be used only for
   IME (input method editor) composition/accessibility integration, never as the renderer.

## Consequences

### Positive

- Correct shaping and bidi across scripts.
- Deterministic text for golden tests.
- The same text stack works on Vulkan, WebGPU, and the software path.

### Negative / risks

- Several dependencies with different licenses (FreeType, HarfBuzz, SheenBidi, ICU).
- ICU is large; consider a reduced build or optional subset.
- Color emoji and font fallback are additional work.
- Shaped-run caches must be bounded; unbounded caches are a memory leak in disguise.

## Alternatives considered

- **stb_truetype only.** Rejected: no shaping, no bidi, insufficient for production text.
- **Platform APIs (DirectWrite/CoreText).** Rejected: not cross-platform, not deterministic.
- **Skia's text stack (SkParagraph).** Rejected as the primary stack: it would couple text to
  the Skia decision ([ADR-004](ADR-004-buy-vector-path-rasterization.md) (vector path rasterization)) and complicate the WebGPU path. Revisit if Skia is adopted for
  vector paths.
- **HarfBuzz + FreeType without ICU.** Possible for V1; ICU is kept for segmentation/bidi
  correctness but can be made optional behind a feature flag.

## Follow-up

- Spike text rendering in [Big Step B](../AGENTS.md#big-steps-group-work-dont-micro-step) (core renderer): shaped run -> atlas -> quad batch.
- Add a text golden corpus (LTR, RTL, Arabic, Devanagari, emoji fallback).
- Document font licensing and the font files used by tests.
