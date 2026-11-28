# Library farm plan: components and charts

Status: proposed execution plan. The catalog is normative in
[`component-library.md`](component-library.md) section 3 and the stages in its section 8; this file
does not restate either. It answers the question the catalog does not: **in what order the farm
builds the library, and what "done" means for one component.**

## 1. What the parity harness proves today

[`chrome-parity.md`](../guides/chrome-parity.md) compares the engine against Chrome for the same
markup and the same stylesheet. On the parity form the **structure difference is 0.12 %** and the
**pixel difference about 4 %**, and the guide attributes the pixel gap to text rendering (the
engine's glyphs are lighter than Chrome's). Two pages exist: `form` and `dash`.

That is the strongest evidence the project has that the CSS engine is real. It is also the only
place where "does this look right" is answered by a number instead of an opinion.

The gap: **two pages cover four components** (`Button`, `Field`, `Form`, `SafeArea`), and stage 1
of the catalog is **42 components of which 4 exist**. Nothing in the current process forces the
next component to be compared with a browser at all.

## 2. The parity gate

Every component lands with a parity page, or it does not land. Concretely, one component's
definition of done is the ADR-047 item 10 list **plus** a parity run:

| Requirement | Evidence |
|---|---|
| Accessibility contract (role/name/state/actions) | semantics snapshot test |
| Headless interaction test | keyboard and pointer through the real `InputRouter` |
| Gallery page | `samples/gallery` page, all variants and states |
| Documented parts | `::part()` names in the catalog |
| Goldens per theme variant | `tests/golden/gallery/` |
| **Chrome parity** | `tools/parity/<page>.html` twin, `report.json` with `structure_diff_pct` and `differing_pixels_pct` |

Thresholds, to be confirmed by the first run of each page rather than guessed: **structure
difference under 0.5 %** (the form is at 0.12 %), and the pixel difference recorded but not gated,
because it is dominated by text anti-aliasing and would gate on the font, not the component.

A component whose parity page cannot reach the structure threshold is not "almost done": it is
either an engine gap (file it against the engine) or a component bug. Both are findings, and both
are worth more than a component that merely renders.

## 3. Phase order

Ordered by dependency, not by catalog order. Counts are stage-1 components still missing (38) plus
`Icon`, which is a stage-0 catalog item that does not exist yet.

### Phase 0 — unblock the farm (no components)

Four items, all of which otherwise collide across every later phase:

1. **Gallery page dispatch is a hardcoded `if`-chain** (`samples/gallery/main.cpp`, the
   `args.page == "form"` / `"dash"` / `"safe-area"` block) plus per-page `TESSERA_GALLERY_*_CSS`
   compile definitions in `samples/gallery/CMakeLists.txt`. Every new parity page edits both, so
   two farm agents cannot work on two components at once. Replace it with a page table before
   phase 1.
2. **Parity page convention.** **Decided 2026-11-18: both are supported.** `chrome_parity.mjs`
   keeps the `form` module path (a CSS module needs `--emit-module-css` first, #463) and the `dash`
   plain path. A component page uses whichever its stylesheet already is; no page is rewritten to
   fit the tool, and the gate script (#529) handles both shapes.
3. **A parity gate script.** One command that builds the engine capture, runs Chrome, compares, and
   exits non-zero above the structure threshold, so the farm can run it without reading a guide.
4. **The bundled font pack (#280).** `font-weight` is parsed and carried but **bold is not compared**
   (the parity guide says so outright). The pack (Plex Sans 400/600/700, Mono 400/700) exists in
   `packages/tessera-fonts/ibm-plex/`, and the parity gate and the chrome-parity harness now use its
   Plex Sans Regular, SemiBold and Bold (#970, #973: `--font` repeats and the text stack picks a face by
   weight). No twin page sets a bold weight yet, so bold is loadable but not yet compared.

### Phase 1 — text and layout (14)

`Icon`, `Text`, `Heading`, `Label`, `Kbd`, `Code`, `Divider`, `Spacer`, `VisuallyHidden`, `Stack`,
`Grid`, `Card`, `ScrollView`, `AspectRatio`.

Cheapest and highest-confidence: text and boxes are what the parity harness already measures well.
`Icon` comes first because `IconButton`, `Link`, `Banner`, `EmptyState` and `Chip` all consume it,
and because the `tessera icons add` tool (#238) exists while the component does not.

### Phase 2 — actions (4)

`IconButton`, `ToggleButton`, `SegmentedControl`, `Link`.

`Button` already proves the pattern (variants, sizes, disabled, `onPress` behind a guard), so these
are variations on a solved shape. `SegmentedControl` is the one with real work: the parity form
already exercises it, so its page is half-written.

### Phase 3 — inputs (10)

`TextInput`, `TextArea`, `PasswordInput`, `SearchInput`, `NumberInput`, `Checkbox`, `RadioGroup`,
`Switch`, `Slider`, `Select`.

The hardest phase and the one where parity pays most. These need caret and selection paint, focus
rings, disabled and error states, and `::part()` styling. Known engine-side risks to resolve here
rather than work around:

- `Slider`'s pointer drag is unblocked (#532): a pointer handler reads node-local `e.x` and the width
  from `onSizeChange` ([common-and-events section 3.1](../specs/jsx-intrinsics-v1/common-and-events.md)).
  Wiring it into the primitive is a phase 3 item.
- `TextInput` single-line centring and the `text-align` single-line fast path are both recent fixes
  with open follow-ups; the parity page is the regression test for them.

### Phase 4 — feedback (7)

`Spinner`, `Progress`, `Skeleton`, `Badge`, `Chip`, `EmptyState`, `Banner`.

Mostly paint. `Skeleton` is the first component that wants a gradient shimmer, which is now
possible: `background-image: linear-gradient()` from a stylesheet landed (#457, PR #519). Radial
and conic gradients are still `Later` in `stylesheet-v1.md` section 11, so a radial shimmer is out.

### Phase 5 — media and overlay (3)

`Image`, `Avatar`, `Tooltip`.

`Image` needs the decode-at-display-size rule (issue #14) and the bounded decode/texture caches
that already exist. `Tooltip` is the first overlay consumer; `OverlayHost`, placement, dismissal
and the `InputPump` filter landed in #302, so the engine half is done.

### Phase 6 — data (1)

`List`.

`src/ui/virtual/` already holds the windowing, recycling, scroll anchoring, key index and item
state store, so `List` is a component over an existing engine. `Table` and `DataGrid` are stage 3
and 4 and stay out of V1.

## 4. Charts

Charts are not a component phase; they are a separate track with an unusual shape: **the engine
half exists and the component half does not.**

What exists (`include/tessera/ui/chart/`, all tested):

- `chart_series.hpp` — fixed-capacity ring buffer, allocation-free after construction (#182)
- `chart_node.hpp` — retained series geometry plus the damage a rebuild implies (#183)
- `chart_geometry.hpp` — per-segment device-space boxes, rebuild only what an append moved
- `chart_render.hpp` — retained emission plus the immediate-mode escape hook
- tests: `chart_series_ring`, `chart_node`, `chart_node_damage`, `chart_node_equivalence`

What does not exist: **no chart component in `packages/tessera-ui/`, no axis, no grid lines, no
legend, no tooltip or crosshair, and no sample.** `samples/` has no chart page and the parity
harness has no chart page.

Three decisions blocked the chart track. **Two are now answered** (2026-11-18):

1. **How a polyline is drawn.** **Answered:** ThorVG is ON by default
   ([ADR-057](../../decisions/ADR-057-vector-paths-thorvg-default-and-exit.md)), so a polyline is a
   `PathShape` through `PathRasterizer` like any other vector. No new `PaintSink` method and no
   per-chart triangle-strip code.
2. **Pie and donut need an arc.** **Answered by the same decision:** ThorVG rasterizes arcs, so pie
   and donut are unblocked. `PaintSink` still has no arc primitive, and that is now a deliberate
   choice rather than a gap: arcs go through the path seam.
3. **`filter` and `clip-path` are `Later`.** Still open. [`stylesheet-v1.md`](../../docs/specs/stylesheet-v1.md)
   section 11 lists both as `Later`. A chart that wants a glow, a drop shadow on a line, or a
   clipped plot area cannot have one. Record this in the chart spec rather than discovering it per
   component.

Suggested chart order once the decisions land: **Sparkline → Line → Area → Bar → Pie/Donut**, with
the axis, legend and crosshair built once as shared parts rather than per chart. The streaming
dashboard sample (#184, open, V1-M6) is the acceptance artifact and should be the chart track's
parity page.

## 5. Farm mechanics

- **One issue per component**, filed from the decided set, each carrying the parity gate above.
  #240 is a tracking issue and cannot be picked; it should be closed in favour of the per-component
  issues once the set is decided (#527).
- **Labels:** `ready` only when the component's engine prerequisites exist; `blocked` with the exact
  missing decision otherwise. Phase 0 items are `ready` now.
- **Grouping for CI:** phases 1, 2 and 4 are safe to land several components per PR (same files,
  same subsystem). Phase 3 components each touch the input path and should land one per PR.
- **Shared files stay with the Lead:** `samples/gallery/main.cpp`, `samples/gallery/CMakeLists.txt`,
  `tests/unit/CMakeLists.txt`, `docs/README.md`, `AGENTS.md`. Phase 0 item 1 exists to remove the
  first two from that list.
- **Docs:** `component-library.md` is 338 lines against a 600-line hard limit, so the catalog can
  grow, but each component's parts and states belong in its own page or a split section rather than
  in that file.

## 6. Decisions

### Decided 2026-11-18 (owner)

| # | Decision | Effect |
|---|---|---|
| 1 | **ThorVG is ON by default** ([ADR-057](../../decisions/ADR-057-vector-paths-thorvg-default-and-exit.md)) | Unblocks `Icon` (#530) and the chart polyline and arc questions (#531) at once. The Windows lane pins it OFF for its build budget, so the no-ThorVG configuration stays tested. |
| 2 | **If ThorVG does not measure up, Tessera implements its own path system in-tree and reduces the dependency over time** (ADR-057 decision 3) | The exit is a **measured** trigger, not a feeling: determinism, correctness, cost (2x the spike envelope), maintenance or licensing. The seam makes the migration one `PathBackend` value plus one file. |
| 3 | **The web gallery (#290) is in V1** | Stage 1's exit condition ("published for native and web") is now a V1 commitment, not a follow-up. |
| 4 | **Parity pages support both CSS modules and plain CSS** | `chrome_parity.mjs` keeps the `form` module path (`--emit-module-css`) and the `dash` plain path; a component page picks whichever its stylesheet already is. No page has to be rewritten to fit the tool. |

### Still open

| # | Decision | Blocks |
|---|---|---|
| 5 | The concrete V1 widget set (#527) | every phase |
| 6 | `Slider` pointer drag: decided, node-local `e.x` plus `onSizeChange` width (#532) | none |

### The ThorVG exit triggers (ADR-057 decision 3)

Recorded here so the farm knows the exit is a decision, not a task. Any one of these, with the
measurement attached, opens a decision issue; it does not start an in-house rasterizer on the spot.

| Trigger | Measurement |
|---|---|
| Determinism | the same path set is not byte-identical across runs or across the two gating lanes |
| Correctness | a committed-corpus path renders wrong and the pinned version cannot fix it |
| Cost | more than 2x the spike envelope: 12.2 s build, 894 KiB Release, 1.7 ms first frame at 512x512 |
| Maintenance | the pin cannot be upgraded for two release cycles, or the CPU engine is dropped |
| Licensing | the license stops being permissive, or ADR-012 static linking cannot be met |

## 7. Risks

- **The parity gate is the plan's load-bearing assumption.** If a component's page cannot be
  compared because the engine lacks a primitive, the gate turns into a queue of engine issues. That
  is the correct outcome, but it means phase 3 and the chart track will produce engine work, not
  just components, and should be scheduled with that in mind.
- **Text anti-aliasing dominates the pixel number.** Do not gate on `differing_pixels_pct`; it
  measures the font, not the component. The structure number is the one that catches layout bugs.
- **Breadth is the failure mode this plan is trying to avoid.** The comparison set (RmlUi, eepp,
  AffineUI) all have complete widget sets; the first two ship in real products and the third has
  stalled. Tessera's defensible differences are the compiler, the single RHI, determinism and the
  accessibility bridge — none of which a 42-component library demonstrates. The plan should be read
  as "enough components to prove the engine on a real screen", not "catch up on widget count".
