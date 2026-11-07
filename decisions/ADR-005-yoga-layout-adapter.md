# ADR-005: Yoga for Flexbox, with a small Grid subset

- Status: Accepted
- Date: 2026-11-07
- Related: ADR-003, ADR-006

## Context

Tessera exposes a CSS-like layout model. Flexbox is the primary layout for UI panels, toolbars,
and lists; Grid is secondary (dashboards). Implementing Flexbox from scratch is well-known to
be error-prone: intrinsic sizing, min/max constraints, wrapping, gap, baseline alignment, RTL,
absolute positioning, and percentage resolution interact in subtle ways.

Yoga is the production Flexbox engine used by React Native. It is MIT-licensed, C++,
cross-platform, and designed for exactly this use case.

## Decision

1. Use **Yoga** as the Flexbox engine. Do not write a Flexbox implementation in V1.
2. Put a `LayoutEngine` adapter in `src/ui/layout/` that translates our style tokens
   (`flex-direction`, `justify-content`, `align-items`, `gap`, `padding`, `min/max`,
   percentages, absolute positioning) into Yoga nodes.
3. Text measurement is injected into Yoga through a callback backed by the text stack
   (ADR-006). Layout must not rasterize text.
4. **Grid**: implement a small CSS-grid subset on top of Yoga or vendor **taffy** behind a
   C ABI if needed. V1 supports:
   - fixed/repeat columns and rows
   - gaps
   - item span
   - basic alignment
   Not in V1: subgrid, masonry, auto-placement with complex dense packing.
5. **Integer-pixel paint alignment**: retain fractional geometry through layout, then round
   once at paint time using a single documented rounding rule. Golden tests must use the same
   rule.
6. The adapter is the only place that knows about Yoga. UI code never includes Yoga headers.

## Consequences

### Positive

- Battle-tested Flexbox behavior.
- Text measurement integration is a known problem with documented solutions.
- Swapping engines later is possible because only the adapter depends on Yoga.

### Negative / risks

- Yoga's CSS behavior differs from browsers in edge cases (baseline, percentage resolution).
  Document divergences as they are found.
- An extra dependency and build target.
- Grid remains our code and therefore our bug surface; keep it small in V1.

## Alternatives considered

- **Write Flexbox ourselves.** Rejected: high cost, high risk, no differentiation.
- **Use taffy (Rust) for both Flexbox and Grid.** Rejected for V1: adds a Rust toolchain and
  FFI, and Yoga is already proven in production.
- **Use a browser layout engine (Blink/WebKit).** Rejected: far too heavy; contradicts the
  "no browser in production" goal.
- **Pixel-absolute layout only.** Rejected: unusable for responsive UI.

## Follow-up

- Write `docs/design/layout.md` covering the style mapping and rounding rule.
- Build a layout test corpus (JSX source + expected boxes) before adding Grid.
- Add Yoga to `third_party/` with its license note.
