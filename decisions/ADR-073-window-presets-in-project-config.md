# ADR-073: Window presets declared in tessera.json

- Status: Proposed (2026-11-25).
- Date: 2026-11-25
- Related: [project-config-v1](../docs/specs/project-config-v1.md) (the `window` block, versioning
  rule), [os-events-v1](../docs/specs/os-events-v1.md) section 2 (`FoldInfo`, the hinge angle),
  [safe-area-v1](../docs/specs/safe-area-v1.md) (the hinge as an inset), [designer-studio-v1](../docs/specs/designer-studio-v1.md) sections 5 and 6 (skins, device
  presets, `viewport.set`), [responsive](../docs/design/responsive.md) section 2.1 (the window is the
  media context, issue #436), [ADR-020](ADR-020-window-resize-and-lifecycle.md) (window resize and
  lifecycle), [ADR-045](ADR-045-browser-hosted-studio.md) (simulated chrome is a skin).
- Terms: a **preset** is a named, data-only window size (plus the input facts a simulation needs).
  **Logical pixels** are the client area before the DPI scale. A **bundled skin** is one of the skins
  shipped with the Studio under `designer/web/public/skins/`.

## Context

`tessera.json` has one `window` block with one size (`width`, `height`, default 1280x800). A project
that is responsive needs several canonical sizes, and today they live in three places that do not
share anything:

1. **The Studio** offers a fixed list of four viewports (`designer/web/src/lib/preview-state.js`,
   `VIEWPORTS`) and the bundled skins. They are the same for every project, so a project cannot say
   "my phone layout is 360x780" or "my kiosk is 1920x1080".
2. **Headless runs** (`--frames`, harness frame capture, golden scenes) take a size from flags or from
   the test, and each test restates it.
3. **The first native window** reads `window.width/height` and nothing else.

The mechanism a preset needs already exists. The window is the media context: a size and a scale
change what `@media` answers, and a resize re-lays the tree out and re-resolves the cascade only when
a breakpoint is crossed ([responsive](../docs/design/responsive.md) section 2.1). The Studio's device
preset already drives that path through `viewport.set`. What is missing is only a place in the
project to name the sizes.

Two earlier decisions bound the answer. The Studio ships bundled skins only; user-provided skins are
not in V1 (owner decision, 2026-11-11). And `tessera.json` is the source of truth for the project
([project-generator](../docs/guides/project-generator.md)), so a second file that also carries sizes
would be a second source.

## Decision

### 1. `window.presets`: a list of named sizes

An optional array of at most 16 entries. A preset is data and nothing else:

| Field | Required | Meaning |
|---|---|---|
| `id` | yes | `^[a-z][a-z0-9-]*$`, unique in the list |
| `label` | no | Display text, at most 64 characters; defaults to the id |
| `width`, `height` | yes | Integers from 1 to 16384, logical pixels |
| `scale` | no | DPI scale, a number above 0 and at most 8; defaults to `window.scale`, and `auto` means 1 in a run with no real display |
| `skin` | no | A bundled skin id; the Studio draws that frame, everything else ignores it |
| `foldPosture` | no | `flat`, `book` or `tabletop` (the HAL's set, `parse_fold_posture`); absent means no fold |
| `hingeAngle` | no | The simulated hinge angle in degrees, 0 (closed) to 360, as `FoldInfo::hinge_angle_degrees`; absent leaves it unknown (`-1`, `kUnknownHingeAngle`) |
| `pointer` | no | `fine` or `coarse`: the `pointer`/`hover` answers a simulation gives |

`foldPosture` and `hingeAngle` are independent, as they are in the HAL and on a device: the posture is
a discrete state that picks the hinge geometry and relayouts, the angle is a continuous reading
([os-events-v1](../docs/specs/os-events-v1.md) section 2). A preset may give either, both or neither.

There is no `orientation` field. Orientation is derived from width and height, and an orientation
swap is a swapped size, which is how the Studio already treats it. Theme and colour scheme are not
part of a preset either: they are separate toggles, and a preset that also fixed them would multiply
the list.

### 2. `window.preset`: which one the first window uses

An optional preset id. When present, the first window takes that preset's size and scale.
`window.preset` and an explicit `window.width` or `window.height` are mutually exclusive: naming both
is an error rather than a silent precedence. With no `window.preset`, behaviour is exactly what it is
today.

### 3. Window bounds: `minWidth`, `minHeight`, `maxWidth`, `maxHeight`

Four optional integers in `window`, logical pixels, each at least 1. They bound the window, not a
preset:

- A native window asks the platform for them (a minimum and maximum tracking size). The HAL has no
  such support today, so that is part of the implementation, not an assumption.
- The Studio's custom size entry clamps to them and says so.
- Every preset, and the initial `window.width/height`, must lie inside them; a preset outside is an
  error rather than a silent clamp, so a headless frame never differs from the size its preset names.
- `min` greater than `max` on one axis is an error, and so is any bound together with
  `window.resizable: false`, because a fixed-size window has nothing to bound.

### 4. What reads presets

- **The Studio** lists the project's presets first, by label, then its built-in viewports. Choosing
  one sends the same `viewport.set` size, `dpr`, `foldPosture` and pointer facts a built-in preset
  sends, so it re-resolves `@media` through the existing path. A `skin` the Studio does not know is
  ignored with a warning, never fetched.
- **Headless and native runs** accept `--window-preset <id>` on the generated `main` and on the
  harness frame commands. An unknown id is a usage error that lists the declared ids.
- **`--window-preset all`** runs every declared preset in one invocation, in declaration order, and
  writes one frame per preset (`<name>.<id>.png`). Each preset starts from a fresh evaluation, so one
  preset's state cannot leak into the next. It fails, and names the preset, if any one fails; with no
  presets declared it is a usage error. The flag is
  `--window-preset`, not `--preset`, because `--preset` already means a CMake preset.
- **A real window uses size and scale only.** `skin`, `foldPosture`, `hingeAngle` and `pointer` describe a
  simulation; a native window reports its real input capabilities and its real fold state, and a
  preset never overrides them.

### 5. The fold angle, and which CSS names are ours to use

Checked against the W3C specifications on 2026-11-25. The standard features for foldables are:

- `device-posture: continuous | folded`, a media feature of the Device Posture API (Candidate
  Recommendation Draft, 17 November 2026, section 6.1). It takes no min/max prefix.
- `horizontal-viewport-segments` and `vertical-viewport-segments`, integer range media features of
  Media Queries Level 5 (Working Draft, 19 February 2026, sections 4.7 and 4.8): the number of logical
  regions a fold or hinge divides the viewport into.
- `env(viewport-segment-width|height|top|left|bottom|right <x> <y>)`, environment variables of CSS
  Environment Variables Module Level 1 (First Public Working Draft, 23 September 2025, section 2.3).

**No CSS specification has an angle.** The Device Posture API had a fold-angle value and removed it
(its change log: "Remove fold angle value from the specification", PR #58). Its privacy section says no
low-level sensor detail is exposed and that there is no one-to-one mapping from a sensor reading to a
posture. The angle is therefore not a CSS concept today, and this ADR does not invent one: no
`hinge-angle` media feature, no `env(hinge-angle)`, no new property.

What follows for Tessera:

- **Standard names, when Tessera exposes folds to CSS.** The simulated and real folds map to the
  standard features and to nothing of ours: `flat` is `device-posture: continuous`; `book` and
  `tabletop` are `folded`; a vertical hinge gives `horizontal-viewport-segments: 2` and a horizontal
  hinge `vertical-viewport-segments: 2`; the segment rectangles come from `FoldInfo::hinge_bounds`
  through `env(viewport-segment-*)`. I found none of these in the engine's stylesheet code or specs
  today, so this is new surface, but it is the standard's. Today a fold reaches the UI only through
  the safe-area inset ([safe-area-v1](../docs/specs/safe-area-v1.md)).
- **The preset values stay simulation inputs, not CSS values.** `foldPosture` (`flat`, `book`,
  `tabletop`) and `hingeAngle` are the HAL's vocabulary (`parse_fold_posture`, `FoldInfo`) used to
  drive a simulation. The names a stylesheet sees are the standard ones above.
- **The angle reaches CSS as a Tessera variable, not a W3C name.** The engine already carries it:
  Android fills `hinge_angle_degrees` from `Sensor.TYPE_HINGE_ANGLE`, and it changes on
  `HingeAngleChanged` rather than `FoldChanged` so a slow fold does not relayout per degree.
  [ADR-074](ADR-074-stylesheet-environment-w3c-names-and-tessera-prefix.md) exposes it as
  `env(--tessera-hinge-angle)`, a host-defined `--` variable that cannot collide with a future
  standard name, usable only in paint-only properties.
- **Simulation (decided here).** The Studio's postures leave the angle unknown, so a project cannot
  review its design at 90 degrees. A preset's `hingeAngle` sets it, and the Studio gains a continuous
  angle control that sends a new `hingeAngle` field through `viewport.set`. Changing the angle runs
  no layout pass by itself, exactly as on a device.

### 6. Not a runtime feature

An end user does not pick a preset and the app does not switch presets while running. A preset sets a
starting size or a simulated one. A setting an end user may change belongs in `tunables`
([project-config-v1](../docs/specs/project-config-v1.md)), not here.

### 7. Validation

Validation extends the `tessera.json` rules and uses the strict schema: an unknown preset field is an
error. These are errors, with proposed codes under `TESSERA_PROJECT_WINDOW_PRESET_`: a duplicate or
malformed `id`, a size or scale out of range, more than 16 presets, a `foldPosture` or `pointer`
outside its set, a `hingeAngle` outside 0 to 360, a preset or the initial size outside the window
bounds, `min` above `max`, a bound together with `resizable: false`, a `window.preset` that names no preset (`..._UNKNOWN`), and `window.preset` together
with `window.width/height` (`..._CONFLICT`). An unknown `skin` is not a project error because the
skin set belongs to the Studio, which reports it.

### 8. Schema version

Adding optional fields keeps `schemaVersion` at `1` ([project-config-v1](../docs/specs/project-config-v1.md),
Versioning). An older tool rejects the new field because unknown fields are errors; that is the
strict schema working as designed.

## Consequences

- One place names a project's sizes. The Studio, a headless frame run and a native dev run agree,
  and a golden scene can be reproduced by preset name instead of by a copied number pair.
- The Studio has to read the project's `window` block. The preview package already carries
  `tessera.json`, so this is a parser change, not a new file.
- The surface grows by the preset fields, four bounds and a flag, all optional. A project that ignores them is
  unchanged.
- `skin`, `foldPosture`, `hingeAngle` and `pointer` do nothing on a real window, which could surprise
  someone who expects a native run to behave like the preview. Section 4 says it, and the flag's help
  text must too.
- A foldable design can be reviewed at a chosen angle in the Studio before any device exists, but
  a stylesheet cannot read the angle until [ADR-074](ADR-074-stylesheet-environment-w3c-names-and-tessera-prefix.md) is accepted and implemented.

## Alternatives considered

- **Keep presets inside the Studio.** This is the status quo. Headless and native runs cannot reach
  them, so every test keeps restating a size. Rejected.
- **A separate `presets.json`.** It would be a second source of truth for sizes next to
  `tessera.json`, and the Studio's own `presets.json` (a built-in list of skin combinations) would be
  confused with it. Rejected.
- **`window.breakpoints`.** Breakpoints are app-authored in CSS `@media` rules and carry meaning;
  declaring them again in JSON invites drift. Presets are inputs to a run, not semantics. A
  Studio ruler that reads the stylesheet's own breakpoints is a separate idea.
- **User-provided skin files.** Out of V1 by the 2026-11-11 owner decision; `skin` therefore names a
  bundled id only.

## Follow-up

Implementation slices, one landing each, to be filed when this is accepted:

1. Schema, parser, diagnostics and unit tests for `window.presets`, `window.preset` and the four
   bounds, plus the first window taking a preset's size and the platform min/max tracking size.
2. The Studio toolbar reading the project's presets, the clamp on custom sizes, and the angle control
   with `viewport.set` `hingeAngle`.
3. `--window-preset <id>` and `--window-preset all` on the generated `main` and the harness frame
   commands.
4. Update `project-config-v1`, `designer-studio-v1` sections 5.3 and 6, `os-events-v1` and the `new`
   templates.

[ADR-074](ADR-074-stylesheet-environment-w3c-names-and-tessera-prefix.md) covers exposing folds and the window
environment to stylesheets: the standard names where they exist and `--tessera-` variables where they
do not. It must not add a CSS name the W3C has not defined outside that prefix.
