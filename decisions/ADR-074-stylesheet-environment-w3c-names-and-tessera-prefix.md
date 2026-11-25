# ADR-074: Stylesheet environment: W3C names where they exist, `--tessera-` names where they do not

- Status: Proposed (2026-11-25).
- Date: 2026-11-25
- Related: [ADR-073](ADR-073-window-presets-in-project-config.md) (simulated fold posture and hinge
  angle in window presets; its section 5 is the reason this ADR exists),
  [ADR-023](ADR-023-os-event-normalization-and-foldables.md) (`FoldInfo`),
  [os-events-v1](../docs/specs/os-events-v1.md) section 2, [safe-area-v1](../docs/specs/safe-area-v1.md),
  [responsive](../docs/design/responsive.md) section 2.1 (re-resolve only when an answer can change),
  [use-media-query-v1](../docs/specs/use-media-query-v1.md),
  [animation-runtime](../docs/design/animation-runtime.md) (paint-only frames),
  [permissions-v1](../docs/specs/permissions-v1.md).
- Terms: an **environment variable** is a document-global value read with `env()`. A **segment** is one
  of the logical regions a separating fold divides the viewport into. A **discrete** value changes
  rarely and may affect layout; a **continuous** value changes every frame while a user moves a
  device and may only affect painting.

## Context

The engine knows things a stylesheet cannot ask. `WindowMetrics` carries the safe-area and IME and
gesture insets, the text scale, and a `FoldInfo` (state, orientation, occlusion, `separating`, hinge
rectangle, and the continuous hinge angle that Android fills from its sensor). Today the fold reaches
the UI only through the `SafeArea` component, the angle reaches nothing in `src/ui`, and the engine has
no `env()`: `MediaContext` has no fold field and value resolution has no environment function.

The W3C already defines part of this, and ADR-073 checked it on 2026-11-25:

- Media features: `device-posture: continuous | folded` (Device Posture API, Candidate
  Recommendation Draft, 17 November 2026, section 6.1), and `horizontal-viewport-segments` and
  `vertical-viewport-segments` (Media Queries Level 5, Working Draft, 19 February 2026, sections
  4.7 and 4.8).
- `env(<name> [, <fallback>])` and a closed list of variables (CSS Environment Variables Level 1,
  First Public Working Draft, 23 September 2025): `safe-area-inset-top|right|bottom|left`,
  `viewport-segment-width|height|top|left|bottom|right <x> <y>`, and `preferred-text-scale`.

There is no CSS angle. The Device Posture API removed its fold-angle value (PR #58) and says no
low-level sensor detail is exposed. And the Environment Variables draft is strict about names: it
lists the UA-defined variables and says "additional UA-defined environment variables must not be
supported unless/until they are added to this list". The same draft reserves `--`-prefixed names
(custom-property names) for variables the user or author defines. That is the room an app framework
has: it is not a user agent defining new names, it is the host providing global variables.

## Decision

### 1. Three media features, with the standard names and values

Added to `MediaFeature` and answered from `MediaContext`, so `@media`, `useMediaQuery` and
`useBreakpoint` share one evaluator, as they do for `pointer` and `hover`:

| Feature | Answer |
|---|---|
| `device-posture` | `folded` when `FoldState` is `HalfOpened`; `continuous` for `Flat` and `None`. Exactly one of the two is always true, as the spec requires. No min/max prefix |
| `horizontal-viewport-segments` | 2 when the fold is `separating` and its orientation is `Vertical` (book: panes side by side); otherwise 1 |
| `vertical-viewport-segments` | 2 when the fold is `separating` and its orientation is `Horizontal` (tabletop: panes above and below); otherwise 1 |

Both segment features take the range forms the engine already parses. A fold that does not separate is
one segment, which is how Media Queries defines it: a region split off by a hardware feature that acts
as a logical divider. `MediaContext` gains the fold's discrete fields only, and the cascade re-resolves
when an answer flips, by the existing rule (`PreparedStyleSheet::media_answer_can_change`).

**The angle never enters `MediaContext`.** That is an invariant, with a test: a `HingeAngleChanged`
leaves the context byte-equal, so a slow fold never re-resolves the cascade per degree.

### 2. `env()` in the stylesheet engine

`env(<name> [, <fallback>])` is added to value resolution with the standard's rules: the fallback
(everything after the first comma) is used when the variable does not exist; with no fallback and no
variable the declaration is invalid at computed-value time, which drops it and reports a diagnostic;
a syntactically valid `env()` makes the whole property valid at parse time.

### 3. Which names, and the rule for the ones the W3C has not defined

**Standard names, unprefixed, only the ones the standard lists:**

| Variable | Source in the engine |
|---|---|
| `safe-area-inset-top|right|bottom|left` | `hal::resolve_safe_area` with the default config (the `SafeArea` component's default sources) over the window's metrics |
| `viewport-segment-width|height|top|left|bottom|right <x> <y>` | the segment rectangles derived from `FoldInfo::hinge_bounds` when the fold separates; one segment (the whole viewport) otherwise |
| `preferred-text-scale` | the OS text scale in `WindowMetrics` |

**Names the W3C has not defined use the `--tessera-` prefix, as author-defined variables.** They are
`<custom-property-name>`s, so they sit in the space the standard gives to the host, and they never
claim a name the standard might later define:

| Variable | Value | Source |
|---|---|---|
| `--tessera-hinge-angle` | `<angle>` in `deg`, 0 (closed) to 360 | `FoldInfo::hinge_angle_degrees` |
| `--tessera-ime-inset-top|right|bottom|left` | `<length>` | `WindowMetrics::ime_insets` |
| `--tessera-gesture-inset-top|right|bottom|left` | `<length>` | `WindowMetrics::gesture_insets` |

An unmeasured angle (`kUnknownHingeAngle`, `-1`: a desktop, iOS, the web build, a preview with no
`hingeAngle`) means the variable does not exist, so the `env()` fallback applies:
`transform: rotate(env(--tessera-hinge-angle, 180deg))`. A name outside both tables is a nonexistent
variable, not an error, which is what the standard says about unknown names.

**Replacing a prefixed name with an official one** is a rename, by design. When the W3C defines the
real variable (an official hinge angle, `keyboard-inset-*` for the IME insets), the official name is
added, the `--tessera-` name becomes an alias that reports a deprecation diagnostic, and it is removed
at the next breaking stylesheet revision. The set above is closed: a new name needs an entry here or a
follow-up ADR, so the prefix never becomes a place to put anything.

### 4. Two kinds of value, and what each may feed

- **Discrete** (the standard variables, `--tessera-ime-inset-*`, `--tessera-gesture-inset-*`): a
  change can affect layout. The prepared sheet records which variable names it references, so a
  change re-resolves the cascade only when the sheet references that name, and a sheet that does not
  use `env()` pays nothing. Allowed in any property.
- **Continuous** (`--tessera-hinge-angle`): resolved at sample time each frame, like an animated
  value, with no style or layout stage. It is allowed only inside properties the animation runtime
  treats as paint-only (`transform`, `opacity`, colour; `property_dirty.cpp` is the map). In a layout
  property it is invalid at computed-value time with a diagnostic. This keeps a slow fold from
  becoming a layout input, and it matches how posture and segments, not the angle, are the layout
  switches. `HingeAngleChanged` coalesces into the newest, so a fast fold is one paint-only update per
  frame, and a static UI still idles with zero presents.

There is no `useHingeAngle` hook. One mechanism is enough: a component that wants an angle-driven
effect writes it in CSS, and a TSX `style` value that carries `env()` goes through the same resolver
(whether the JSX subset's style values may carry the function is checked in the first slice).

### 5. Simulation

ADR-073's `hingeAngle` and the Studio's angle control feed `viewport.set`, the preview builds
`FoldInfo` from the posture and the angle, and every variable and feature above answers from it. A
posture alone leaves the angle unknown, so the fallback applies. The posture-to-fold mapping stays in
`tessera/hal/fold_posture.hpp`, the one place, so the Studio, the native preview and MCP
`set_viewport` agree.

## Consequences

- A stylesheet responds to a fold and to the window's insets with names a web developer already knows,
  and CSS written for the standard parts carries over.
- The angle is usable, in CSS, today, under a name that cannot collide with a future standard one, and
  swapping it for an official one is a mechanical rename with a deprecation diagnostic.
- `env()` is new engine surface (parser, resolver, dependency tracking), the largest cost here.
  `safe-area-inset-*` and `preferred-text-scale` come with it, and the `SafeArea` component and the
  env path then read the same resolver.
- The angle can animate a hinge-following element but cannot change layout. A layout change at a fold
  uses the posture or the segment features, which is the platform's own judgement of the threshold.
- Relying on `--` names for host-defined variables leans on a part of the draft that is still open
  (it notes that how authors add environment variables is not yet defined). If the W3C settles it
  differently, the `--tessera-` names are the ones that change.

## Alternatives considered

- **Bare `tessera-hinge-angle` (no `--`).** Rejected: it would be a UA-defined variable outside the
  standard's closed list, which the draft says must not be supported.
- **`(min-hinge-angle: 100deg)` or an unprefixed `env(hinge-angle)`.** Rejected: no W3C spec defines
  them, the Device Posture API removed its angle on purpose, and a media feature on a continuous
  sensor would re-resolve the cascade per degree.
- **A hook (`useHingeAngle`) instead of, or as well as, CSS.** Rejected for now: a second mechanism
  for the same value, and a readable number would let a component branch on it and turn the angle into
  a layout input. CSS with the paint-only rule gives the same effect with one path.
- **Posture only, no angle.** Enough for layout, but the engine already pays for the sensor and a
  hinge-following effect is what it is for.
- **Standard variables only, no `--tessera-` ones.** Leaves the angle and the IME and gesture insets
  unreachable from CSS, which is the gap this ADR exists to close.
- **Map `book` and `tabletop` to Tessera-specific media values.** Rejected: the standard
  `device-posture` has two values and the segment features carry the axis.

## Follow-up

Slices, one landing each, to be filed when this is accepted:

1. `MediaContext` fold fields, the three media features, parser and evaluator tests, the
   byte-equal-on-angle test, and the spec rows in `stylesheet-v1` and `use-media-query-v1`.
2. `env()`: the function, fallback and invalid-at-computed-value-time rules, the referenced-name set
   on the prepared sheet, and the standard variables (`safe-area-inset-*`, `preferred-text-scale`,
   `viewport-segment-*`).
3. The `--tessera-` variables, the continuous/discrete rule and the paint-only diagnostic, plus the
   Android feed for the angle.
4. The preview and host fill the fold and environment from `FoldInfo` and the metrics in one shared
   function, with the Studio and MCP agreeing.

Open questions for the owner: should reading the hinge angle be a declared permission in
`permissions` (that spec lists no sensor category today and says nothing is on by default); and
should `titlebar-area-*` and `keyboard-inset-*`, which other W3C drafts define and I have not
checked, be added to the standard table once someone has read those specs.
