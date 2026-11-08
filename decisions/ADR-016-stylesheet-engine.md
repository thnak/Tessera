# ADR-016: Stylesheet engine — full CSS syntax with staged semantics

- Status: Proposed (direction accepted; parser/buy decision pending)
- Date: 2026-11-07
- Related: [ADR-005](ADR-005-yoga-layout-adapter.md) (Yoga layout adapter), [ADR-009](ADR-009-deterministic-headless-golden-tests.md) (deterministic headless golden tests), [ADR-014](ADR-014-project-config-format.md) (project config format), [ADR-017](ADR-017-input-and-fixed-step-runtime.md) (input and fixed-step runtime loop), [ADR-018](ADR-018-dirty-rendering-and-frame-pacing.md) (dirty rendering and frame pacing)

## Context

Tessera exposes a CSS-like UI model. [ADR-005](ADR-005-yoga-layout-adapter.md) commits to Yoga for Flexbox and a small Grid
subset; the execution plan (a planning document that is not in this repository) describes a controlled CSS subset. The product direction is now a
**full CSS engine** with web stylesheet support, while keeping native rendering deterministic
and browser-free.

A literal implementation of every CSS module is unbounded: selectors, cascade, inheritance,
custom properties, at-rules, media/container queries, color spaces, flex/grid, transitions,
animations, and vendor quirks. A browser engine cannot be embedded without violating the
native/deterministic constraints. Stylo/Servo would add Rust, a large runtime, and a DOM model
that conflicts with the [scene graph](../docs/reference/glossary.md).

## Decision

1. Split **syntax** from **semantics**:
   - parse full modern CSS syntax (CSS3/4-era) into a stable AST;
   - implement semantics according to a versioned compatibility matrix;
   - unsupported features parse, are preserved, and emit diagnostics.
2. Build a custom tokenizer, parser, selector matcher, and cascade engine in `src/ui/style/`.
   No browser, DOM, JavaScript, or platform text API dependency.
3. Match selectors against a minimal `StyleNode` interface (tag/id/classes/attributes/
   pseudo-states/parent/siblings). The scene graph adapts to it; no HTML DOM.
4. Implement the cascade with origins (UA = user agent, user, author, inline), `!important`, specificity,
   source order, inheritance, and custom properties with `var()` fallback/cycle handling.
5. Expand shorthands and validate values through a property registry that produces a typed
   `ComputedStyle` (layout, paint, animation groups).
6. Use the same engine at build time and runtime. Build-time may precompile the AST/rule table;
   runtime must produce identical results for identical inputs.
7. Determinism rules: stable source/selector order, fixed viewport/DPI, no wall clock, no
   hash-order dependence.
8. Dirty integration: style changes set `StyleDirty`; the resolver raises `LayoutDirty`/
   `PaintDirty` only when the corresponding computed group changed.
9. If full semantic coverage becomes an unsustainable maintenance burden, isolate a mature
   parser/cascade (for example `libcss`) behind the `CssParser` interface and record a
   follow-up ADR. Do not embed a browser engine or Stylo in V1.
10. [V1](../docs/reference/glossary.md) semantic priorities: box model, Flexbox, small Grid subset, typography, colors,
    borders/radius/shadows, 2D transforms, transitions/keyframes, `@media`, `@supports`,
    `@layer`, `@font-face` parsing, custom properties. Not in the V1 priority list (no release is named): `:has()`, `@container`,
    `@scope`, `@property`, wide-gamut colors, full Grid.

## Consequences

### Positive

- Existing `.css` files and web-style authoring are viable.
- Build-time and runtime share one deterministic implementation.
- Unsupported features degrade with diagnostics instead of failing the whole stylesheet.
- The compatibility matrix makes progress measurable and reviewable.

### Negative / risks

- Full syntax parsing plus a selector/cascade engine is a large, long-lived component.
- "Full CSS" can be interpreted as every module; the matrix is the contract, not a slogan.
- Modern color, container queries, and `:has()` have subtle invalidation costs.
- Build-time precompilation adds a second serialization format to keep compatible.

## Alternatives considered

- **CSS subset only.** Rejected by product direction; retained as the compatibility-matrix
  staging mechanism.
- **Browser/Blink/WebKit CSS engine.** Rejected: native determinism, size, and architecture.
- **Servo Stylo.** Rejected for V1: Rust runtime, DOM integration, size, MPL-2.0.
- **libcss (NetSurf).** Kept as a fallback parser/cascade behind the interface if custom
  maintenance becomes too costly; it does not cover all modern CSS semantics.
- **Tailwind-like utility tokens only.** Rejected: not a stylesheet engine and not web-compatible.

## Follow-up

- Implement tokenizer/parser/AST and a conformance corpus.
- Implement selector matching, specificity, cascade, inheritance, custom properties.
- Publish the compatibility matrix in [`docs/specs/stylesheet-v1.md`](../docs/specs/stylesheet-v1.md)
  and keep it current.
- Add parser fuzzing and deterministic computed-style dumps to CI (continuous integration).
- Decide build-time precompilation format (binary AST vs generated tables).
