# Component library and design system

Status: design; a product layer; the library ships after V1 (decided 2026-11-08; V1 is defined in the [glossary](../reference/glossary.md), staging is in [ADR-032](../../decisions/ADR-032-bundled-component-library.md)). Decisions: [ADR-032](../../decisions/ADR-032-bundled-component-library.md) (bundled component library), amended by [ADR-047](../../decisions/ADR-047-component-library-layers-and-design-language.md) (layers, theme versioning, Instrument design language, catalog and stages).

The framework provides primitives; a bundled component library makes it usable without every
team rebuilding inputs, dialogs, menus, and data grids.

## 1. Scope

- A separate package built on public Tessera APIs, not part of the core renderer.
- Source components compile to the same C++/scene graph; no JS runtime.
- Accessible by default; themable via design tokens; deterministic for goldens.
- Ships source + generated C++, not opaque binaries.

## 2. Design tokens

- Colors, typography, spacing, radii, elevation, motion, breakpoints, z-layers.
- Semantic tokens (`surface`, `on-surface`, `danger`, `focus-ring`) over raw palette.
- Themes: light/dark/high-contrast; token overrides per app/surface.
- Token changes are style-only; a change that only edits a token value does not require recompiling components. Which other token changes (renames, new tokens) do require it is not specified.
- Tokens are versioned; visual changes require golden updates.

## 3. Catalog

Stage numbers refer to section 8. Behavior primitives (press, toggle, focus scope, roving focus,
dismissable layer, list/grid navigation, typeahead) are stage 0 and are not listed per component.

| Group | Components (stage) |
|---|---|
| Foundations | Icon (0); Text, Heading, Label, Kbd, Code, Divider, Spacer, VisuallyHidden, SafeArea (1); AnimatedIcon (2) |
| Actions | Button, IconButton, ToggleButton, SegmentedControl, Link (1); SplitButton, CopyButton, FloatingActionButton (2) |
| Inputs | TextInput, TextArea, PasswordInput, SearchInput, NumberInput, Checkbox, RadioGroup, Switch, Slider, Select (1); RangeSlider, PinInput (2); ComboBox/Autocomplete, TagInput, DatePicker, TimePicker (3); DateRangePicker, ColorPicker, FilePicker/Dropzone, Rating (4); RichTextEditor, CodeEditor (5) |
| Forms | Field, Form (1, moved to TSX by issue [#70](https://github.com/thnak/Tessera/issues/70)); FieldSet, ValidationSummary (2) |
| Layout | Stack, Grid, Card, ScrollView, AspectRatio (1); Accordion/Collapsible (2); Splitter, TwoPane (3); Masonry (4); DockingLayout (5) |
| Navigation and shell | Tabs, AppBar, Sidebar/NavigationRail, BottomNavigation, Drawer, Breadcrumb, MenuBar, Toolbar, StatusBar, Navigator (2); Pagination, Stepper/Wizard, CommandPalette (3) |
| Overlays | Tooltip (1); Dialog, AlertDialog, Sheet/BottomSheet, Popover, HoverCard, Menu, ContextMenu, Toast, LiveCapsule (2) |
| Feedback and status | Spinner, Progress (linear/circular), Skeleton, Badge, Chip/Tag, EmptyState, Banner/Alert (1); Meter/Gauge (3) |
| Data display | List (1); Table, Tree, Timeline, Stat/KPI tile, Descriptions (3); DataGrid, TreeTable, PropertyGrid, Calendar (month/week/day) (4); Spreadsheet, Kanban (5) |
| Charts | Line, Bar, Area, Pie/Donut, Sparkline, Gauge (3); Scatter for large data, Candlestick/OHLC, Heatmap, Histogram, Box plot, real-time streaming with decimation, zoom/pan/crosshair, multi-axis (4); charts inside Spreadsheet, 3D charts (6) |
| Media | Image, Avatar (1); Carousel (2); QRCode (3); ImageViewer with zoom/pan (4); Video, AudioPlayer, Lottie (5); CameraPreview, QRScanner, PDFViewer, Map (6) |
| Motion and transitions | AnimatePresence (enter/exit), ViewTransition, SharedElement (2); PullToRefresh, InfiniteScroll, Sortable (drag and drop) (3) |

- DataGrid builds on `VirtualGrid` (see [`virtualization-v1`](../specs/virtualization-v1.md)), selection, sorting, column resize/reorder, keyboard nav.
- Forms use [`forms-and-validation-v1`](../specs/forms-and-validation-v1.md); overlays use
  [`overlays-and-layers-v1`](../specs/overlays-and-layers-v1.md).
- ViewTransition, SharedElement and Navigator transitions wrap
  [`view-transitions-v1`](../specs/view-transitions-v1.md) ([ADR-035](../../decisions/ADR-035-view-transitions.md)).
- Charts are drawn with vector paths ([ADR-004](../../decisions/ADR-004-buy-vector-path-rasterization.md)); 3D and map components use custom elements ([`custom-elements-v1`](../specs/custom-elements-v1.md)), not core changes.
- Image decodes at display size times device pixel ratio (issue [#14](https://github.com/thnak/Tessera/issues/14)); Video and AudioPlayer use the platform media services of [`media-and-export.md`](media-and-export.md).
- `AppBar` and `WindowChrome` are framework shell components, not library components: they ship in
  the runtime, are imported from `"tessera"`, and are specified in
  [`window-chrome-v1`](../specs/window-chrome-v1.md) section 8.

### 3.1 Icons and animated icons (Iconify)

- Icons come from [Iconify](https://iconify.design/) icon sets, pinned by version and imported at
  **build time** (`tessera icons add <set>:<name>`). Apps never fetch icons from the Iconify API at
  run time: builds stay deterministic, offline and free of third-party requests.
- **License gate** from each set's metadata: MIT, ISC, Apache-2.0, BSD, CC0 and OFL-1.1 are
  accepted; CC-BY is accepted with attribution written to the generated notices; share-alike sets
  need an explicit opt-in; GPL-family, non-commercial and unknown licenses are rejected.
- Static icons become vector paths (ADR-004) and are tree-shaken. `currentColor` maps to the text
  color token; multi-color icons keep their palette unless `monochrome` is set.
- **AnimatedIcon** converts the SVG SMIL animations that animated Iconify sets use, plus CSS
  `@keyframes` inside the SVG, into Tessera keyframes at build time. Supported subset: `opacity`,
  `stroke-dasharray`, `stroke-dashoffset`, fill and stroke colors, path `d` when both shapes have
  the same command structure, `animateTransform` rotate/translate/scale, `begin` offsets and
  `<id>.end` chaining, `repeatCount`, `keyTimes`, `keySplines`, and linear/spline/discrete
  `calcMode`. Anything else reports `TESSERA_ICON_ANIMATION_UNSUPPORTED` and falls back to the
  static first frame.
- Animated icons run on the animation runtime: they respect reduced motion (end state shown),
  pause when hidden and keep render-on-demand.
- Lottie animations are a separate component (stage 5), not part of the icon path.

### 3.2 Safe areas, notches and folds

- `SafeArea` applies the insets from `SafeAreaChanged` and `FoldInfo`
  ([`os-events-v1`](../specs/os-events-v1.md)): notches and display cutouts, rounded screen
  corners, status/navigation bars, home indicator and the on-screen keyboard. An `edges` prop
  selects which sides apply.
- AppBar, BottomNavigation, Drawer, Sheet and Toast apply safe areas by default; content can opt
  out to draw edge to edge.
- `TwoPane` places content on each side of a hinge or fold and becomes one pane on unfolded or
  single screens.

### 3.3 LiveCapsule (floating status capsule)

- A pill-shaped overlay anchored at the top center of the window that shows ongoing activity
  (timer, playback, upload or download progress, call, recording) without leaving the current
  screen. It is an in-app component, not the operating system's live-activity feature.
- **Placement**: on devices with a top display cutout it wraps the cutout, so the capsule reads as
  an extension of the hardware; elsewhere it sits under the top safe-area inset. This needs the
  cutout **rectangles**, not only insets: `os-events-v1` gains cutout bounds from Android
  `DisplayCutout` bounding rects; platforms without them (iOS, desktop, web) fall back to top
  center under the safe area.
- **States**: `minimal` (icon or dot), `compact` (leading and trailing slots around the cutout) and
  `expanded` (a card with content and actions). Tap expands, tapping outside or swiping up
  collapses. Size and corner radius morph between states with the spring runtime; reduced motion
  switches states with a cross-fade.
- **Activities**: an app-level queue with priority; at most one compact activity plus one
  minimal one are visible, others wait. Each activity owns its content through slots and ends
  explicitly or on a timeout.
- **Accessibility**: a polite live region announces start and end of an activity; the capsule is
  focusable, has a name from the active activity, and its expanded actions are reachable by
  keyboard. It never steals focus.
- **Power**: an idle capsule draws nothing new; progress updates follow render-on-demand.
- Mirroring activities into system live activities (iOS Live Activities, Android ongoing
  notifications) is a stage-6 platform integration.

### 3.4 Spreadsheet compared with DataGrid

- DataGrid shows typed records: one row per record, one column per field.
- Spreadsheet is cell-addressed (`A1`): range selection, fill handle, frozen rows and columns,
  merged cells, copy/paste as TSV, undo/redo and a deterministic formula subset (arithmetic,
  `SUM`, `AVERAGE`, `MIN`, `MAX`, `COUNT`, `IF`, references and ranges). Both share `VirtualGrid`.
- File import/export (`.xlsx`, `.csv`) is an optional package (stage 6), subject to the license
  rules in [`dependencies.md`](../specs/dependencies.md).

### 3.5 Field and Form IR generation (issue #70)

- `tessera/components/Field.tsx` and `tessera/components/Form.tsx` are the single source of truth for
  the first two library components: their names, `comp::` ids, declared prop records, destructuring
  defaults and node trees. Nothing builds their IR by hand.
- The build runs `tessera_codegen --emit-library-cpp` over both files (CMake lists the two sources as
  explicit dependencies) and compiles the generated `library_components_generated.cpp` into
  `tessera_library_components`. The public `library_components()` / `find_library_component()` /
  `make_library_component()` contract in `library_components.cpp` delegates to that registry.
- A source that does not lower cleanly (parse, analysis, lowering, or an unsupported default) fails
  the build; there is no hand-built fallback. The generator is a separate host executable linked
  against the lowering core, not the generated registry, so the data can depend on the generator
  without a build cycle. Cross-compiling builds cannot run the generator and compile a committed
  copy; every host build compares that copy with the freshly generated unit and fails loudly when it
  is stale.
- Defaults are authored where React puts them, as destructuring defaults on the props parameter
  (`label = ""`, `validators = []`, `required = false`); lowering stores them as `Prop::default_value`
  and codegen initializes the generated field with them. `?` in the props alias remains the separate
  "may be omitted" flag.
- Determinism: the generated file has no timestamps or absolute paths, and identity is stamped by
  `assign_library_identity` from the fixed `tessera.library` salt: a component id is a hash of its
  source path and name, and a node id a hash of its source path and structural path
  ([`automation-ids-v1.md` section 2.1](../specs/automation-ids-v1.md)). No index-derived band is
  used, so adding a library component does not move the ids of the existing ones; the committed
  codegen goldens pin the stamped numbers.
- `tests/unit/library_tsx_parity_tests.cpp` re-lowers the committed TSX in-process and compares the
  result with the embedded IR (stamped identity excluded), including spans, defaults and text-run
  shape. It is the staleness guard: editing a TSX source without rebuilding the embedded data fails
  the suite, as does regenerating it from a changed source.

## 4. Contracts

- Every interactive component: role/name/state/actions, keyboard behavior, focus ring,
  disabled/read-only, touch target, text scale, high contrast, reduced motion.
- Controlled/uncontrolled patterns are consistent across inputs.
- Overlay components follow focus trap/restore/dismiss rules.
- Loading/empty/error states are explicit, not ad-hoc.
- No component reaches into app globals or raw OS/GPU handles.
- Composition over configuration; slots/children are deterministic.

## 5. Theming and density

- Token-based; no hardcoded colors/spacing in components.
- Density modes (comfortable/compact) affect spacing/touch targets explicitly.
- RTL (right-to-left)/bidi (bidirectional text)-aware; icons mirror only when their meaning is directional.
- Platform conventions (keyboard shortcuts, menu order) are overridable.

## 6. Testing

- Unit: state/interaction/keyboard/focus/validation.
- Semantics snapshots (dump of the accessibility tree, see [`accessibility.md`](accessibility.md)) + accessibility checks (names/roles/actions).
- Golden (reference-image, see [glossary](../reference/glossary.md)) tests: default/dark/high-contrast, compact/expanded, RTL, text scale.
- Headless (no window; see [`headless-automation-v1`](../specs/headless-automation-v1.md)): keyboard/pointer/IME (input method editor)/overlay/data-grid scripts.
- Visual regression across themes/densities; no real GPU baselines.

## 7. Packaging

- `tessera-ui` package with source, generated code, tokens, docs, examples.
- SemVer (semantic versioning); visual breaking changes documented with migration/golden updates.
- Tree-shake at build time; unused components not linked.
- Licenses/fonts/icons packaged and documented.

## 8. Roadmap

Defined by [ADR-047](../../decisions/ADR-047-component-library-layers-and-design-language.md).
Each stage ends when every listed component meets the definition of done (accessibility contract,
headless interaction test, gallery page, documented parts, goldens per theme variant), the gallery
is published for native and web, and the stage's starter template builds only on public library
components.

| Stage | Goal | Contents (section 3) | Starter | Prerequisites |
|---|---|---|---|---|
| 0 | Foundations | Package imports and Field/Form move (#70), `::part()`, Instrument tokens and fonts, behavior primitives, Icon with the Iconify import tool, gallery skeleton | none; exit is one styled Button end to end, shown in the web gallery | front end, stylesheet engine |
| 1 | A settings screen | Core controls, inputs, layout, feedback, SafeArea, Image, List, Tooltip | Settings | stage 0 |
| 2 | A multi-screen app | Overlays, LiveCapsule, app shell (MenuBar, Toolbar, StatusBar, AppBar, Sidebar, Tabs), Navigator with ViewTransition/SharedElement, AnimatedIcon, Carousel | App shell | overlays spec, ADR-035, cutout bounds in `os-events-v1` |
| 3 | CRUD and simple dashboards | Table, Tree, Splitter, TwoPane, ComboBox, Date/TimePicker, CommandPalette, basic charts, Stat tile, Sortable | List-detail CRUD, dashboard | vector paths (ADR-004) |
| 4 | Desktop tools and monitoring | DataGrid, TreeTable, PropertyGrid, Calendar, ColorPicker, Dropzone, advanced charts, ImageViewer | Admin/monitoring console | virtualization, clipboard |
| 5 | Media, documents and editors | Video, AudioPlayer, Lottie, Spreadsheet, RichTextEditor, CodeEditor, DockingLayout, Kanban | Media or document app | media services, multi-style text runs, formula engine |
| 6 | Specialized | Map, 3D wrappers, CameraPreview, QRScanner, PDFViewer, spreadsheet file import/export, 3D charts, LiveCapsule mirroring to system live activities | none | custom elements, optional packages, platform services |

## 8.1 Detail design

Token schema (proposal):

```json
{
  "tokenVersion": 1,
  "color": {
    "surface": { "light": "#ffffff", "dark": "#121212" },
    "onSurface": { "light": "#111111", "dark": "#f2f2f2" },
    "danger": { "light": "#b3261e", "dark": "#f2b8b5" },
    "focusRing": { "light": "#0b57d0", "dark": "#a8c7fa" }
  },
  "space": { "xs": 4, "sm": 8, "md": 12, "lg": 16, "xl": 24 },
  "radius": { "sm": 4, "md": 8, "lg": 12 },
  "type": { "body": { "size": 14, "line": 20, "weight": 400 } },
  "motion": { "fast": 120, "normal": 200, "slow": 320 }
}
```

- Tokens are semantic; raw palette is internal. Components never hardcode colors/spacing.
- Theme selection (`light`/`dark`/`highContrast`) resolves tokens; per-surface overrides are
  scoped and versioned.
- `tokenVersion` follows SemVer; renames/major visual changes require migration + golden review.

Component API contract:

```cpp
struct ButtonProps {
    std::string label_key;     // i18n key, not raw untranslated text
    ButtonVariant variant;
    bool disabled;
    bool loading;
    std::function<void()> on_activate;  // generated binding
};
```

- Consistent controlled/uncontrolled pattern for inputs; no mixed state ownership.
- Every component exposes `disabled`, focus behavior, and a name/label contract.
- Slots/children are explicit; no hidden context/global state.
- Density (`comfortable`/`compact`) changes spacing/touch targets only through tokens.
- RTL uses logical start/end; icons declare `mirror` when directional.

DataGrid initial scope (ships after V1, see the status line):

- Virtualized rows/columns, selection, sorting, column resize/reorder, keyboard navigation.
- Editing/grouping/pivot are deferred with no release assigned (listed in section 9); the core does not depend on them.
- Headless mode exposes rows/cells for tests; semantic table/grid roles.

Testing contract:

- API/state tests, keyboard/focus, semantics snapshot, theme/density/RTL/text-scale goldens.
- No component ships without an accessibility contract and a headless interaction test.
- Visual changes require reviewed goldens; semantic token changes are versioned.

Gap-closure decisions:

- **G-25/Q-24** (gap and question ids in [`app-completeness-gap-resolutions-v1.md`](../architecture/app-completeness-gap-resolutions-v1.md)): token JSON v1 is semantic and SemVer-versioned; component API contract includes
  controlled state, focus, and accessibility; the initial DataGrid supports virtual rows/columns,
  selection, sort, resize/reorder, keyboard. Editing/grouping/pivot are deferred.
- Components ship styled by default on top of public behavior primitives, with `::part()` styling
  and eject (ADR-047); fonts are app assets unless the app opts into the bundled font pack.

## 9. Deferred/open questions

- Lottie: ThorVG's Lottie loader versus a separate library, after a size and license check.
- Spreadsheet formula engine scope beyond the stage-5 subset.
- Map tile provider and tile licensing.
- Video on Linux: GStreamer is LGPL, which conflicts with static linking ([ADR-012](../../decisions/ADR-012-static-linking.md)).
- DataGrid editing/grouping/pivot.
- Bundled font pack/versioning.
- Token rename migration tooling.
- 3D component wrappers.
