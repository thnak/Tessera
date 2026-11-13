# Stylesheet v1 specification

Status: **proposed** (Big Step B2, the runtime slice; see [`AGENTS.md`](../../AGENTS.md#big-steps-group-work-dont-micro-step)).
Normative for `ui/style`. The CSS tokenizer (offsets, comments, escapes, strings, numbers,
dimensions, percentages, functions, at-keywords, hashes, match tokens, Unicode ranges) is
implemented in `src/ui/style/tokenizer.cpp`, and the tolerant rule/declaration/at-rule parser in
`src/ui/style/parser.cpp`; selector parsing/specificity is implemented in
`src/ui/style/selector.cpp`, with right-to-left matching and id/class/tag indexing in
`src/ui/style/selector_match.cpp`, and the origin/importance/specificity/source-order cascade plus
inheritance/computed-style model in `src/ui/style/cascade.cpp`; the top-level pipeline
(parse -> selector index -> cascade -> computed style) is in `src/ui/style/compiled_stylesheet.cpp`.
The property registry (supported properties, initial values, box/pair/flex shorthand expansion,
and diagnostics) is `src/ui/style/property_registry.cpp`; CSS color parsing/SRGB interpolation is
`src/ui/style/color.cpp` (named/hex/rgb/rgba/hsl/hsla). At-rule application for `@media`
(width/height/resolution/orientation/color-scheme/reduced-motion), nested media rules, `@keyframes`
parsing, and `@layer` precedence (plus the generated `Theme` origin) is in
`src/ui/style/media_query.cpp`, `src/ui/style/keyframes.cpp`, `src/ui/style/supports.cpp`, and
`src/ui/style/compiled_stylesheet.cpp`. `@supports` evaluates registry-level declarations with
`not`/`and`/`or`. The semantic matrix and animation runtime remain proposed.
Decision: [ADR-016](../../decisions/ADR-016-stylesheet-engine.md) (stylesheet engine).

Terms: **V1** is the first release scope ([`AGENTS.md`](../../AGENTS.md#non-negotiables), item 8);
**V1.1** is the release after V1 (this file names it only for `@property` and `revert*`);
**Later** means planned after V1 with no release specified. **ComputedStyle** is the typed style
after cascade and inheritance. **StyleNode** is the minimal node view the selector matcher sees.
**Yoga** is the Flexbox layout engine ([ADR-005](../../decisions/ADR-005-yoga-layout-adapter.md)).
**Batched 2D** is the quad/SDF (signed distance field) renderer. **Golden** is a deterministic
reference image. **UA** is user-agent. Other project terms: [glossary](../reference/glossary.md).

## 1. Scope

Tessera supports a **full CSS engine architecture** with two deliberately separate layers:

1. **Syntax** — parse modern CSS (CSS3/4-era syntax) without a browser or DOM.
2. **Semantics** — compute a typed style for a scene-graph node according to the
   compatibility matrix in §11.

"Full CSS engine" here means **full syntax + a documented semantic matrix**, not a claim that
every CSS module is implemented in V1. Unsupported properties and at-rules must still parse,
produce diagnostics, and degrade gracefully. A literal implementation of every CSS module is
unbounded and conflicts with the deterministic V1 goal.

The engine is used by both the build-time compiler and the production runtime. It never depends
on a browser, a DOM, JavaScript, or the platform text stack.

## 2. Goals

- Parse real `.css` files, inline style strings, and inline `style="..."` attributes.
- Match selectors against a minimal `StyleNode` (tag, id, classes, attributes, pseudo-states).
- Implement the CSS cascade, specificity, inheritance, custom properties, and `!important`.
- Produce a typed `ComputedStyle` consumed by layout (Yoga), paint (batched 2D), and animation.
- Be deterministic: stable rule order, fixed viewport/DPI, no wall clock, no system fonts.
- Be fast enough for 10k+ nodes: indexed selectors, cached computed styles, dirty invalidation.
- Preserve unknown syntax for forward compatibility; never crash on invalid CSS.

## 3. Non-goals (V1)

- HTML parsing, DOM, browser layout, or JavaScript.
- Full CSS Grid (subgrid, masonry, dense auto-placement) and full CSS Tables.
- Full SVG/CSS path/filter/shader effects.
- Every color space and every `@`-rule.
- CSS nesting and `@scope` evaluation in V1 (they are parsed only; the release that evaluates them
  is not specified, see section 17).
- Web fonts beyond committed test fonts ([ADR-006](../../decisions/ADR-006-text-stack.md), text stack).
- Browser-style quirks modes, vendor prefixes semantics, or UA stylesheet parity.

## 4. Pipeline

```text
source(.css / inline)
  -> tokenizer
  -> parser -> StyleSheet AST
  -> property registry: validate + expand shorthands
  -> selector index (id / class / tag)
  -> match per node -> declarations
  -> cascade (origin, importance, specificity, order)
  -> inheritance + custom properties
  -> ComputedStyle (layout + paint + animation)
  -> dirty flags -> layout / paint / animation
```

Build-time and runtime use the same engine. Build-time may serialize the AST or a precomputed
rule table; runtime must produce identical results for the same inputs.

## 5. Syntax

### 5.1 Tokenizer

Must handle:

- whitespace, comments (`/* ... */`), CDO/CDC (`<!--`, `-->`)
- identifiers, escapes (`\26`, `\0000E9`)
- strings (single and double quoted, escapes, line continuations)
- numbers, integers, percentages, dimensions (`12px`, `1.5em`, `50%`)
- hash tokens (`#fff`, `#id`)
- functions (`rgb(...)`, `var(...)`, `calc(...)`, `url(...)`)
- delimiters and combinators
- Unicode ranges and `@`-keywords
- source offsets for diagnostics

### 5.2 Structure

```css
@import "tokens.css";
@layer base, components;

@media (min-width: 900px) and (prefers-color-scheme: dark) {
  .card:hover > .title {
    color: var(--text-strong);
    padding: 8px 12px;
  }
}

@keyframes pulse {
  from { opacity: 1; }
  to   { opacity: 0.5; }
}

.card {
  display: flex;
  flex-direction: column;
  gap: 8px;
  border-radius: 12px;
  transition: opacity 120ms ease-out;
}
```

### 5.3 Declarations

- `property: value;`
- `property: value !important;`
- custom properties: `--name: value;`
- empty declarations are ignored
- unknown properties (not recognized as CSS at all) are preserved in the AST and reported by the
  parser once per `(property, source)` pair; this is a different event from the per-
  `(rule, property)` "unsupported declaration" diagnostic in section 11

## 6. Selectors

### 6.1 Supported selector grammar

- type: `button`, `*`
- class: `.card`
- id: `#main`
- attribute: `[disabled]`, `[data-role="tab"]`, `[lang|=en]`, `[class~="x"]`,
  `[href^="https"]`, `[href$=".pdf"]`, `[title*="hello"]`
- combinators: descendant (` `), child (`>`), adjacent sibling (`+`), general sibling (`~`)
- pseudo-classes:
  - state: `:hover`, `:active`, `:focus`, `:focus-visible`, `:disabled`, `:checked`,
    `:enabled`, `:required`, `:optional`, `:visited` (no history in V1; always false)
  - structural: `:first-child`, `:last-child`, `:only-child`, `:nth-child(An+B)`,
    `:nth-last-child(An+B)`, `:first-of-type`, `:last-of-type`, `:empty`, `:root`
  - logical: `:not(...)`, `:is(...)`, `:where(...)`
  - `:has(...)` is parsed in V1; evaluation is listed as Later in section 11
- pseudo-elements: `::before`, `::after`, `::placeholder`, `::selection` (parsed; paint support is not listed in section 11 and is not specified); component parts `::part(<ident>+)` are implemented ([`component-parts-v1.md`](component-parts-v1.md))
- `:not()`/`:is()`/`:where()` accept a selector list; `:where()` contributes zero specificity

### 6.2 Matching rules

- Matching is right-to-left for performance.
- A selector list is a disjunction; each complex selector has its own specificity.
- `:root` matches the scene root.
- `:hover`/`:active`/`:focus` are driven by the input pseudo-state dispatcher (section 12, Input).
- Sibling combinators use the scene-graph child order (paint order).
- No shadow DOM, no slots, no `:host` in V1. `::part(<ident>+)` is the only selector form that crosses a component boundary ([`component-parts-v1.md`](component-parts-v1.md), ADR-047 item 3).
## 7. Cascade

### 7.1 Origins

Lowest to highest:

1. user-agent (Tessera's small built-in defaults)
2. user
3. author
4. inline `style="..."`
5. animations (not implemented in the first cascade slice; no release specified)
6. `!important` reverses origin precedence per CSS (important: user-agent > user > author; the
   list above is the order for normal declarations)

### 7.2 Ordering key

For each declaration:

```text
(origin, importance, specificity, source_order)
```

- `!important` wins over normal declarations; among important declarations the origin order
  is reversed per CSS Cascade: user-agent > user > author, and within the author origin an
  inline `style="..."` declaration beats author rules.
- Specificity is `(ids, classes/attributes/pseudo-classes, types/pseudo-elements)`; `::part()` counts as a pseudo-element and its names add nothing.
- `:where()` contributes zero specificity.
- Equal keys are resolved by source order; the later declaration wins.
- Build-time and runtime must use the same stable source ordering.

Sheets from several files enter one cascade in **import-graph order**: depth-first from the app
root, imports visited in source order, each file placed at its first occurrence (post-order). The
sheet order is the high part of `source_order` and the declaration order inside a file is the low
part, so a later sheet wins an otherwise equal key. First-party library CSS is registered inside
`@layer tessera-ui`, so unlayered app CSS beats it regardless of specificity; the combination
algorithm, layer remapping and the `AppHost` registration API are specified in
[`compiled-stylesheet-v1.md`](compiled-stylesheet-v1.md) section 10.

### 7.3 Inheritance

- Inherited by default: `color`, `font-*`, `line-height`, `letter-spacing`, `word-spacing`,
  `text-align`, `text-transform`, `white-space`, `visibility`, `cursor`, `direction`,
  `writing-mode`.
- Non-inherited properties reset to their initial value unless matched.
- `inherit`, `initial`, `unset`, `revert`, `revert-layer` are parsed; `revert*` is a V1.1
  semantic (treated as `unset` with a diagnostic).
- The root inherits from the UA defaults.

### 7.4 Shorthands

The property registry expands shorthands into longhands before the cascade:

- `margin`, `padding`, `border`, `border-radius`, `border-width/style/color`
- `flex`, `gap`, `overflow`, `inset` (top/right/bottom/left), `place-items`, `place-content`
- logical: `margin-inline`, `padding-inline`, `inset-inline`, `border-inline-width/style/color`,
  `border-inline-start`, `border-inline-end` (section 11.1)
- `transition`/`animation` are registered for the cascade and `@supports` only; the animation
  module parses them (section 11). `background` and `font` are not V1.

If a shorthand contains an unsupported component, keep the supported longhands and emit one
diagnostic.

## 8. Values, units, and colors

### 8.1 Lengths

- absolute: `px`, `cm`, `mm`, `in`, `pt`, `pc`, `q`
- font-relative: `em`, `rem`, `ex`, `ch`
- viewport: `vw`, `vh`, `vmin`, `vmax`, `sv*`, `lv*`, `dv*`
- percentage: resolved by the consumer (layout vs paint) with documented rules
- `calc()` with `+ - * /`, nested `calc()`, and `min()`/`max()`/`clamp()`

### 8.2 Other values

- numbers, integers, angles (`deg`, `rad`, `turn`, `grad`), times (`ms`, `s`)
- colors: named colors, `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`, `rgb()`, `rgba()`,
  `hsl()`, `hsla()`, `transparent`, `currentColor`
- modern color functions (`lab()`, `lch()`, `oklab()`, `oklch()`, `color()`) are parsed and
  preserved; semantic support is listed as Later in section 11
- strings, URLs, identifiers, functions
- `var()` with fallback and cycle detection

### 8.3 Resolution boundary

`ComputedStyle` stores typed values in a canonical form. Percentage and `auto` values are kept
symbolic until layout; paint-only values are resolved at paint time. This symbolic-until-layout storage applies identically to
build-time and runtime.

## 9. Custom properties and themes

- Custom properties are inherited by default.
- `var(--name, fallback)` resolves at computed-value time.
- Cycles resolve to the fallback or the property's initial value, with a diagnostic.
- Theme tokens are custom properties defined on `:root` or on a theme scope.
- `@property` registration is parsed in V1.1; until then, all custom properties are untyped
  token streams.
- The tokens of the active theme are injected as a generated `:root` rule with origin `theme` (section 9.1).
- V1 limit: a shorthand whose value contains `var()` is expanded to longhands before substitution
  (shorthands are expanded when the sheet is prepared), so the token must resolve to a single
  component value; write the longhands when the token holds several components. `var()` inside a
  quoted string is a literal string, never a reference.
- A reference with no usable value and no fallback follows `unset` semantics and reports
  `TESSERA_CSS_UNRESOLVED_VAR`; every member of a dependency cycle becomes invalid and reports
  `TESSERA_CSS_VAR_CYCLE` (section 15).

### 9.1 Theme asset

The **theme asset** is the JSON file named by `assets.theme` in [`project-config-v1.md`](project-config-v1.md).
Without it, the project has one theme, the built-in `default`, which defines only the reserved chrome
pair (section 9.2).

```json
{
  "schemaVersion": 1,
  "start": "light",
  "themes": {
    "light": { "--color-bg": "#ffffff", "--color-fg": "#111111" },
    "dark":  { "--color-bg": "#101014", "--color-fg": "#f2f2f2" }
  }
}
```

| Field | Type | Required | Meaning |
|---|---|---|---|
| `schemaVersion` | integer | yes | `1`. Any other value is `TESSERA_THEME_VERSION` |
| `themes` | object | yes | Theme id to token map. An id matches `[a-z][a-z0-9-]*` (`TESSERA_THEME_ID`). A token name starts with `--`; a token value is a string holding a custom-property value (section 9). Nested values are not allowed |
| `start` | string | no | Id of the theme active at start. Default `default`. An id not in `themes` and not `default` is `TESSERA_THEME_START_UNKNOWN` |

- The set of theme ids is `default` plus the keys of `themes`. A `themes.default` entry gives the built-in
  theme its tokens. These ids are what `useTheme()`, `setTheme(id)` and the designer's `viewport.set` accept
  ([`jsx-subset-v1.md`](jsx-subset-v1.md) section 7.1).
- Only the active theme's tokens are injected. When the active theme changes, the injected rule is
  replaced and every node whose computed style uses a changed token becomes `StyleDirty`.
- A token defined in some themes but not in all of them is the warning `TESSERA_THEME_TOKEN_MISSING`
  (naming the theme and token); an undefined token resolves through `var()` fallback as usual.
- **Theme and color scheme are independent.** `prefers-color-scheme` reports the operating-system
  preference (`light` or `dark`; in the designer, the viewer's `colorScheme` input), never the active
  theme id, and changing the theme does not change it. A project that wants one theme per scheme writes
  `@media (prefers-color-scheme: dark)` rules, or calls `setTheme` from an effect that depends on
  nothing but its own state; the framework does not link them automatically in V1.
- The compiler embeds the asset in the generated program; changing it needs a rebuild, and the designer
  reloads it on `asset.update`.
- The embedded form is the versioned theme asset blob of
  [`compiled-stylesheet-v1.md`](compiled-stylesheet-v1.md) section 11; the runtime loads it without
  parsing JSON.

### 9.2 Reserved chrome tokens

Two custom properties are reserved because the framework reads them before a render tree exists
(launch artifact, loading screen, window chrome):

| Token | Built-in `default` value | Used by |
|---|---|---|
| `--window-background` | `#ffffff` | P0 launch artifact, P2 loading screen background, system-bar tint |
| `--window-foreground` | `#111111` | loading mark/foreground, light/dark title-bar and system-bar glyphs |

- Values must be plain, fully opaque colours (named, `#hex`, `rgb()`, `rgba()` with alpha 255,
  `hsl()`); `var()`, `calc()`, percentages and lengths are `TESSERA_CHROME_THEME_VALUE` at build
  time, because the pair is resolved from the theme asset before the style engine runs.
- The built-in `default` theme defines both tokens, so a project without a theme asset always has a
  coherent pair. A project theme may override either or both per theme; defining only one is
  `TESSERA_THEME_TOKEN_MISSING` and the other falls back to the built-in value.
- Artifact generation, `window.appearance` and chrome timing are normative in
  [`window-chrome-v1.md`](window-chrome-v1.md).

## 10. At-rules

| At-rule | V1 | Notes |
|---|---|---|
| `@media` | yes | width/height/resolution, orientation, `prefers-color-scheme`, `prefers-reduced-motion` |
| `@supports` | yes | property/value support checks against the registry |
| `@layer` | yes | layer order; unlayered author rules win |
| `@font-face` | parse | font loading is [ADR-006](../../decisions/ADR-006-text-stack.md); V1 uses committed fonts |
| `@keyframes` | parse | animation runtime samples at fixed timestep |
| `@import` | build-time | resolved by the asset/build pipeline |
| `@container` | V1 (engine) | `(min-/max-)width/height/inline-size/block-size` and range form, joined with `and`, optional container name; `or`, `not`, `style()` are diagnosed. Rules apply against the nearest ancestor with `container-type: size \| inline-size` (and a matching `container-name`); a container is not styled by its own query. Inline-size containers answer width only. Re-resolution on a size change is the engine observer of the settle loop ([layout-observation.md](../design/layout-observation.md) section 5.4): only a crossed condition restyles |
| `@property` | Later | parsed; typed custom properties in V1.1 |
| `@scope` | Later | parsed; evaluated later |
| `@page`/`@namespace`/`@charset` | no-op | parsed and ignored |

Unknown at-rules are preserved and skipped with a diagnostic. In the V1 column, `Later` has the
meaning given in the section 11 legend; `parse` means parsed with no semantic effect.

## 11. Compatibility matrix

Legend: **V1** = implemented semantics; **Parse** = parsed/preserved, no semantic effect;
**Later** = planned after V1; the release is not specified. Per registered longhand, with its consumer and proving test, see [`stylesheet-property-status-v1.md`](stylesheet-property-status-v1.md). V1 names, one per line, in [`stylesheet-v1.properties.txt`](stylesheet-v1.properties.txt), kept in sync by a test.

| Area | V1 semantics | Later |
|---|---|---|
| Box model | `display` (block/flex/grid/none), `width/height/min/max`, `margin`, `padding`, `border`, `box-sizing` (default `content-box`, as in CSS), `aspect-ratio`, `overflow`, `field-sizing` (`fixed`, `content`; `content` makes a `TextArea` a measured leaf, see [layout-observation.md](../design/layout-observation.md) section 6.3) | table, multi-column |
| Scrolling | `overflow`, `scroll-behavior`, `overscroll-behavior`, `overscroll-effect`, `scrollbar-gutter` | scroll-driven animations, CSS `scroll-snap-*` properties (the scroll controller already implements snap physics in V1, configured through `ScrollSnap` in [`scroll-and-gestures-v1.md`](scroll-and-gestures-v1.md); `allow_chain` is derived from `overscroll-behavior`, there is no `scroll-chain` property) |
| Flex | direction, wrap, grow/shrink/basis, justify/align, gap, order | baseline edge cases |
| Grid | `display: grid`, `grid-template-columns/rows` (lengths, percentages, `fr`, `auto`, `min-content`/`max-content`, `minmax()`, `repeat(<integer>, ...)`), item `grid-column`/`grid-row` (line numbers and `span n`), `gap`, `justify-items`/`justify-self`, `align-items`/`align-self`, `justify-content`/`align-content`, `grid-auto-flow` (`row`, `column`); full value list in [`grid-v1.md`](grid-v1.md) | subgrid, masonry, dense auto-placement, named lines/areas, `grid-auto-columns`/`grid-auto-rows`, `fit-content()` |
| Positioning | `static`, `relative`, `absolute`, `fixed`, `sticky` | anchor positioning |
| Paint | `background-color`, `border-radius`, `border-color/width/style`, `box-shadow`, `opacity`, `color`, `backdrop-filter` (`blur`, `saturate`, `brightness`, `contrast`; [`glass-v1.md`](glass-v1.md)) | gradients, images, `filter`, general `clip-path`, lens/refraction |
| View transitions | `::view-transition-*(root)`, `circle()/inset()/ellipse()` `clip-path` on those pseudos only, `--vt-*` | other names, filters/masks; see [`view-transitions-v1.md`](view-transitions-v1.md) |
| Typography | `font-family/size/weight/style`, `line-height`, `letter-spacing`, `word-spacing` (additive per painted ASCII space U+0020 on both measure paths; [`text.md`](../design/text.md) section 6.4), `text-align`, `text-transform` (`none`, `uppercase`, `lowercase`, `capitalize`; ASCII 1:1 mapping in [`text.md`](../design/text.md) section 6.5), `white-space` (`normal`, `nowrap`, `pre`, `pre-wrap`, `break-spaces`; mode semantics in [`text.md`](../design/text.md) section 6.1), `text-overflow` (`clip`, `ellipsis`; one value only, paint-side truncation in [`text.md`](../design/text.md) section 6.3) | full OpenType features, justification |
| Transforms | `transform` functions `translate`, `scale`, `rotate`, `skew`, `matrix` (2D affine) and `transform-origin` | the individual `translate`/`rotate`/`scale` properties, 3D, perspective |
| Transitions | property/duration/timing/delay, fixed-step sampling | spring physics |
| Animations | `@keyframes`, iteration, direction, fill-mode | scroll-driven |
| Interaction | `cursor`, `pointer-events`, `user-select`, `:hover/:active/:focus/:focus-visible` | `:has()`, `:focus-within` |
| Responsive | viewport media queries, rem/vw/vh, `@container` size queries with `container-type`/`container-name` | container style/scroll-state queries, `container` shorthand, container query length units |
| Colors | named/hex/rgb/hsl/currentColor/transparent | lab/lch/oklab/oklch/color() semantics |
| Custom properties | `var()`, inheritance, fallback, cycles | `@property` typed values |
| Layout direction | `direction` (`ltr`/`rtl`, inherited, per node); physical properties (`left`, `right`, `margin-left`, ...) stay physical; the logical inline-axis properties `margin-inline-*`, `padding-inline-*`, `inset-inline-*`, `border-inline-*` and `text-align: start/end` mirror with the computed direction (section 11.1) | scroll and overscroll mirroring (offsets, scrollbars, overscroll effects stay physical) |
| Component parts | `part` attribute, `::part(<ident>+)` with the pre-`::part` compound matching the component instance, `@supports selector()` (section 6.3) | shadow DOM, `exportparts`, `::slotted()`, `:host`/`:host-context()` |

**V1 decisions for the Scrolling row (decided 2026-11-13 with issue #248; the code matches):**

- `scroll-behavior: auto | smooth` is a non-inherited runtime input for the scroll controller.
  Programmatic scroll (`scroll_to_programmatic`/`scroll_by_programmatic`) jumps for `auto` and starts
  a fixed-step exponential approach for `smooth` (time constant 0.1 s, 0.5 px settle; reduced motion
  clamps every smooth request to a jump) — [`scroll-and-gestures-v1.md`](scroll-and-gestures-v1.md)
  section 2.
- `overscroll-behavior: auto | contain | none` is non-inherited and derives the controller's
  `allow_chain`: at a clamped edge a container passes the leftover wheel delta to its nearest
  scrollable ancestor only for `auto`; `contain` and `none` drop it (there is no CSS `scroll-chain`
  property) and do not animate past the edge (animation-v1 section 7).
- `overscroll-effect: none | glow | stretch | bounce | platform` is non-inherited, uses the
  `OverscrollEffect` names of [`animation-v1.md`](animation-v1.md) section 7, and has no `auto`
  keyword: the initial request is `platform`, which the HAL profile resolves. `none`, or a
  non-`auto` `overscroll-behavior`, suppresses the effect; the stretch/bounce visual state itself is
  that spec's animation slice.
- `scrollbar-gutter: auto | stable` is non-inherited and the one scroll property that is layout
  input: `stable` reserves the 8 px overlay scrollbar width on the inline-end edge inside the
  container's content box, so its children narrow by that width even while no scrollbar is painted.
- An invalid value of any of the four is one `StyleDiagnosticKind::UnsupportedValue` diagnostic per
  declaration and the declaration is dropped, so the computed value stays at its initial; a `var()`
  reference survives parsing and is judged after substitution. `@supports` shares the same grammar,
  so `@supports (overscroll-behavior: contain)` evaluates true.

**V1 decisions for the Paint/Transforms rows (decided 2026-11-10; the code matches):**

- `z-index` orders painting (ascending) and hit testing (descending); ties keep child order;
  `auto`/invalid is 0. Stacking is per parent (siblings only); V1 has no stacking contexts, because
  isolating opacity/transform needs offscreen layers the batched renderer does not have. Revisit
  when layer isolation lands.
- `transform` and `transform-origin` (default `50% 50%`) resolve to a node matrix
  (`translate`, `scale`, `rotate`, `skew`, `matrix` and the axis variants, left to right as in
  CSS); an invalid value means no transform. Hit testing and node boxes follow the matrix. Only the
  `transform` functions exist in V1: the compiler and React-style sources never emit the individual
  `translate`/`rotate`/`scale` properties, and adding them changes only the cascade, not what can
  be drawn. 3D functions (`*3d`, `perspective`) are unsupported and reported.
- A sink that reports `PaintSink::supports_transform()` (the Batched2D GPU sink) paints transformed
  subtrees under the real matrix; overflow clips inside them map through scale/translate/quarter-turn
  frames and are dropped under other rotations or skews. The damage-clipping wrapper forwards the
  matrix and replays commands under each device-space region. The CPU sink paints the
  axis-aligned bounding box instead.
- `box-shadow` takes up to 4 comma-separated shadows (offset, blur, spread, color). Blur is a
  3-8 layer Gaussian-matched stack drawn beneath the node; `inset` parses but is not painted, and
  the shadow is not cut out beneath a translucent background (that needs an inverse clip the
  `PaintSink` does not have). Revisit `inset` and the cut-out when the component library needs
  them or an inverse clip exists.
- Value forms V1 ignores on a recognised property (`box-shadow: inset`, 3D `transform`, any
  `backdrop-filter` function other than `blur`/`saturate`/`brightness`/`contrast`) produce a
  `StyleDiagnosticKind::UnsupportedValue` diagnostic per declaration and stay in the AST, like
  other unsupported declarations (below).
- `FramePipeline::sync_style` (between layout and `finish_frame`) copies these values onto the
  scene; nothing in a production loop calls it yet.

### 11.1 Logical properties and the direction contract

`direction` is inherited and stored on every computed style; layout passes it to Yoga per node
(`YGDirectionLTR`/`YGDirectionRTL`, [`layout.md`](../design/layout.md) section 6). A node's logical
longhands resolve against its own computed `direction`, so the same stylesheet mirrors between LTR
and RTL layouts.

| Group | Longhands | Shorthands |
|---|---|---|
| Margin | `margin-inline-start`, `margin-inline-end` | `margin-inline` (1-2 values) |
| Padding | `padding-inline-start`, `padding-inline-end` | `padding-inline` (1-2 values) |
| Insets | `inset-inline-start`, `inset-inline-end` | `inset-inline` (1-2 values) |
| Border | `border-inline-start/end-width/style/color` | `border-inline-width/style/color` (1-2 values), `border-inline-start`, `border-inline-end` (`<width> \|\| <style> \|\| <color>`) |

- The inline-start side is the physical `left` in LTR and the physical `right` in RTL; inline-end is
  the other side. Layout maps the longhands to Yoga's `Start`/`End` edges, which Yoga resolves
  against the node's own direction; paint applies the same mapping to the `border-inline-*` colors
  and to `text-align`.
- Physical properties are unchanged: `left`/`right`, `margin-left`/`-right` and the like always mean
  the physical side, so mirroring is opt-in per property instead of implicit.
- A declared logical longhand wins over the physical longhand that maps to the same side; an
  undeclared one leaves the physical value (or its initial) in effect. Logical longhands therefore
  store no initial value and are absent from `ComputedStyle` until declared; `initial`/`unset` on a
  logical longhand removes the override rather than forcing the physical initial value.
- `text-align: start | end` mirror with `direction`; `left`/`right` stay physical and `center` is
  unchanged. `justify` is not implemented and falls back to the logical start (see
  [`text.md`](../design/text.md) section 6).
- `border-inline-start`/`border-inline-end` reset omitted components to their initial values (`0`,
  `none`, `currentColor`), as shorthands do.
- The logical mapping assumes the V1 horizontal writing mode. Vertical writing modes are parsed but
  not laid out ([`layout.md`](../design/layout.md) section 6).
- Scroll and overscroll mirroring (offset direction in pointer/wheel input, scrollbars, overscroll
  effects) is **not** part of this contract and stays Later; V1 scroll offsets are physical in both
  directions. Locale-driven mirroring uses the resolved base direction ([`i18n-v1.md`](i18n-v1.md)
  sections 4 and 8.1).

Unsupported declarations (recognized CSS that V1 does not implement; distinct from the parser's
"unknown property" report in section 5.3) are **not** silent: they produce one diagnostic per
`(rule, property)` and are kept in the AST for forward compatibility.

**V1 decisions for the Interaction and `aspect-ratio` rows (decided 2026-11-13; issue #247; the code
matches):**

- `pointer-events: auto | none` (initial `auto`, inherited): `none` removes the node itself from
  pointer hit testing, so hover, active, pointer capture, click synthesis and wheel dispatch land on
  whatever is behind it; the subtree is still searched, so a descendant with an explicit `auto` is
  hit again, exactly as in CSS. Painting, keyboard focus, the focus order and the accessibility tree
  are unchanged: `pointer-events` is not `display`/`visibility` and never hides a node from
  assistive technology. The resolved value is copied onto the scene node by
  `style::sync_interaction_style` (`tests/unit/interaction_style_tests.cpp`).
- `user-select: auto | none | text` (initial `auto`, inherited): `none` blocks starting a text
  selection at the node and in its subtree (drag-select included), `text` allows it, and `auto` keeps
  the default (static text is selectable). An editable `TextInput`/`TextArea` always allows
  selection, whatever an ancestor's `none` says, as in CSS. The scene exposes the resolved policy as
  `Node::text_selection_allowed()`; the static-text selection controller it gates is a later slice
  and the editable-field path is the implemented consumer
  (`tests/unit/interaction_style_tests.cpp`).
- `aspect-ratio: auto | <number> | <number> / <number>` (initial `auto`, not inherited): layout maps
  a positive numeric ratio onto Yoga's aspect ratio, so a box with one `auto` dimension gets the
  other from the ratio; an explicit `width` or `height` wins over the ratio, as in CSS. Both ratio
  terms must be positive. `auto` (and an unsupported value) has no effect. The registry entry is
  what makes `@supports (aspect-ratio: 1/1)` true and gives the property its initial value
  (`tests/unit/layout_tests.cpp`, `tests/unit/interaction_style_tests.cpp`).
- `pointer-events` accepts only `auto` and `none`; any other keyword (the SVG-only set:
  `visiblePainted`, `fill`, `stroke`, `bounding-box`, ...) is recognized but unsupported.
  `user-select` accepts only `auto`, `none` and `text`: `all` and `contain` are recognized but
  unsupported. Unsupported forms produce one `StyleDiagnosticKind::UnsupportedValue` per rule and
  property and stay in the AST (section 11's rule above), and the consumer falls back to the default
  behaviour (`auto`, selectable) instead of silently blocking input.

## 12. Integration

- **StyleNode**: `tag`, `id`, `classes`, attributes, `part` names and the nearest component host, parent/siblings, pseudo-state bits, inline style, theme scope.
- **Layout**: `ui/layout` reads layout longhands and maps them to Yoga nodes ([ADR-005](../../decisions/ADR-005-yoga-layout-adapter.md)). See
  [`layout.md`](../design/layout.md).
- **Paint**: the batched 2D renderer reads paint longhands and produces quads/SDF/text. See
  [`renderer.md`](../design/renderer.md).
- **Text**: text longhands feed shaping/line breaking. See [`text.md`](../design/text.md).
- **Input**: the input pseudo-state dispatcher updates `:hover`, `:active`, `:focus`,
  `:focus-visible`, `:disabled`, `:checked`. See [`input.md`](../design/input.md).
- **Animation**: the fixed-step loop samples transitions/keyframes and writes animated values
  into the computed style before layout/paint. See [`animation-v1.md`](animation-v1.md) and
  [`animation.md`](../design/animation.md).
- **Scrolling**: `overflow`, `scroll-behavior`, `overscroll-behavior`, `overscroll-effect`, and
  `scrollbar-gutter` feed the scroll runtime; overscroll stretch/spring-back is visual only and
  never changes offset/layout. See [`scroll-and-gestures-v1.md`](scroll-and-gestures-v1.md).
- **Compiler**: build-time CSS is prepared (media-independent) and serialized as a blob, see
  [`compiled-stylesheet-v1.md`](compiled-stylesheet-v1.md); generated
  C++ references compiled rule ids, not a second parser.

## 13. Determinism

- Stable source order and stable selector order (no hash-map iteration).
- Fixed viewport, DPI, `rem` base, and font metrics in golden mode.
- No wall clock in style computation.
- Media query evaluation uses the fixed viewport/color-scheme/reduced-motion inputs.
- Identical inputs must produce byte-identical `ComputedStyle` dumps in tests.

## 13.1 Dirty and invalidation

Style is the first dirty domain in the runtime pipeline. See
[`dirty-rendering.md`](../design/dirty-rendering.md) for the full model.

- A class/id/attribute/inline-style/part/theme change marks the node `StyleDirty`.
- The input pseudo-state dispatcher marks `StyleDirty` for `:hover`/`:active`/`:focus`/
  `:focus-visible` changes before style resolution.
- The resolver computes the new `ComputedStyle`, then compares the layout and paint groups with
  the previous values:
  - layout group changed -> `LayoutDirty`
  - paint group changed -> `PaintDirty`
  - either -> `RenderDirty`
- Descendants are invalidated only for subtree-affecting changes (visibility, `display:none`,
  transform/clip, theme scope).
- The scroll runtime inputs (`scroll-behavior`, `overscroll-behavior`, `overscroll-effect`) are not
  layout or paint inputs: changing one re-runs neither domain and only changes what the scroll
  controller does on the next request. `scrollbar-gutter` is layout input, so changing it invalidates
  layout (it is in the layout signature) and the boxes below the container move.
- Selector indexes and cached computed styles are invalidated by the change class, not by a
  global rebuild.
- `:hover` chains are recomputed on scene changes, not only on pointer motion.
- `dirtyRendering=off` (see [`runtime-config-v1.md`](runtime-config-v1.md) section 5.4) forces a
  full rebuild for debugging; it must produce identical pixels.

## 14. Performance

- Selector index: id → class → tag buckets; right-to-left matching.
- Inline style and `:root` tokens are cached separately.
- `ComputedStyle` cached per `(node, pseudo-state, theme-scope)`; invalidated by style dirty
  flags, class/id/attribute changes, and theme changes.
- Precomputed shorthand expansion and property ids; no string comparisons in the hot path.
- Target: style resolution for 10k nodes with 1k rules in a single-digit millisecond budget on
  desktop; measure and record in benchmarks.
- No allocations in the steady-state per-frame path after warm-up.

## 15. Diagnostics

```text
TESSERA_CSS_PARSE_ERROR         invalid token/structure
TESSERA_CSS_UNKNOWN_PROPERTY    parsed, no semantic effect
TESSERA_CSS_INVALID_VALUE       value rejected for the property
TESSERA_CSS_UNSUPPORTED_SELECTOR selector parsed but not matched
TESSERA_CSS_UNSUPPORTED_AT_RULE  at-rule parsed but skipped
TESSERA_CSS_VAR_CYCLE           custom-property cycle
TESSERA_CSS_UNRESOLVED_VAR      missing variable with no fallback
TESSERA_CSS_MEDIA_UNSUPPORTED   media feature not implemented
TESSERA_CSS_UNDECLARED_PART     `::part(name)` of a part a library component does not declare
```

Diagnostics carry source file, line/column, rule selector, and property. Build-time diagnostics
fail the build only when configured to do so; runtime diagnostics go to the dev overlay.

## 16. Testing

- Parser corpus: valid/invalid tokens, escapes, comments, strings, functions.
- Selector corpus: specificity, combinators, structural pseudo-classes, `:not/:is/:where`.
- Component parts: the corpus is listed in [`component-parts-v1.md`](component-parts-v1.md) section 8.
- Cascade corpus: origins, `!important`, inline vs stylesheet, layer order, source order.
- Custom properties: fallback, inheritance, cycles, theme overrides.
- Property matrix: parse/validate/shorthand expansion/computed values.
- Determinism: repeated runs produce identical dumps; rule order is stable.
- Golden layout/paint: style → layout → render on the headless path.
- Fuzz the tokenizer/parser with a bounded corpus in CI (not scheduled; no decision yet).

## 17. Open questions

- ~~Build-time precompilation format~~ decided by [ADR-048](../../decisions/ADR-048-precompiled-embedded-stylesheets.md): versioned binary blob, see [`compiled-stylesheet-v1.md`](compiled-stylesheet-v1.md).
- Whether to adopt a mature parser (for example `libcss`) behind `CssParser` if full syntax
  coverage becomes a maintenance burden. Decision must be recorded in a follow-up ADR.
- CSS nesting, `@scope`, `@container`, `:has()` evaluation order and invalidation.
- Full color-management and wide-gamut color handling.
- Typed custom properties (`@property`) and animation of custom properties.
- Window-wide theme switch effects use view transitions (`view-transitions-v1.md`), not per-token
  animation; the update recalc suppresses style transitions.
