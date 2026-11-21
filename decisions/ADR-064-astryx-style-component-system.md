# ADR-064: The first-party UI library follows Astryx's system design: catalogue, anatomy, theme model and behavior hooks

- Status: Proposed (2026-11-20). The direction is the owner's: "the mock should be a Tessera demo, not
  first-party; Astryx's system design is better; no port, copy the catalogue and the anatomy of
  components and themes", and "those hooks are very valuable". The details below are the proposal to
  review. Amends [ADR-047](ADR-047-component-library-layers-and-design-language.md) item 5 (the default
  design language) and refines items 2, 3, 4 and 7. Nothing else in ADR-047 changes.
- Date: 2026-11-20
- Related: [ADR-047](ADR-047-component-library-layers-and-design-language.md),
  [ADR-063](ADR-063-browser-matching-user-agent-defaults.md) (UA defaults, library CSS in
  `@layer tessera-ui`), [ADR-016](ADR-016-stylesheet-engine.md), [ADR-024](ADR-024-native-first-boundaries.md)
  (no JS runtime in production), [ADR-034](ADR-034-interaction-and-overscroll-animation.md),
  [`component-library.md`](../docs/design/component-library.md), issues #612-#622 (engine gaps), #618
  (umbrella).
- Terms: **Astryx** = the open-source design system at <https://github.com/meta/astryx> (MIT, React 19
  and StyleX), cloned and read at design time only. **Role token** = a token named for what it does
  (`background-surface`, `text-primary`), not for its value. **Target** = one painted part of a
  component that a theme can style. **Hook** = a behavior function a component or an application calls
  for state, focus, keyboard or accessibility; in Tessera it is a contract implemented by the engine.

## Context

ADR-047 chose a bespoke default look ("Instrument": flat, hairline borders, 4 px grid) and three
layers. A React and Vite mock of it, then a second tactile version (`tools/reference/instrument-mock`),
showed two things:

1. A distinctive look is a **theme**, not the library. The tactile version needs several effects the
   engine cannot paint yet (inset shadows, radial and conic gradients, masks, text shadows; issues
   #612-#617) and says nothing about which components exist, how they are built or how they are
   themed. Those questions block shipping, the look does not.
2. A mature system already answers them. Astryx has about 130 components in a documented catalogue, a
   per-component anatomy and contract, a token vocabulary of 211 core tokens, a theme model that
   compiles to plain CSS custom properties, and a set of behavior hooks. Reading it showed what a
   first-party library needs and in what order.

Astryx is not portable as code: it is React and StyleX, and Tessera has no JS runtime in production
(ADR-024). Its design is portable: names, structure, contracts and rules. Copying that structure saves
the design work, the part that takes longest.

## Decision

1. **Instrument stops being the default first-party design language.** The tactile mock stays in the
   repository as a **demo** (a Tessera sample and a candidate theme, "Device"), not as a contract on
   the library. The default look is a neutral, Astryx-like theme built on the token model of item 5;
   Instrument tokens may become a second theme later. ADR-047 item 5 is replaced by this item.
2. **Use Astryx's system design, not its code.** We take the structure (catalogue, anatomy, token
   vocabulary, theme model, behavior contracts, rules) and write our own specs, names where ours must
   differ, and implementation. No Astryx source, docs text or assets are copied into the repository.
   The reference commit and the license are recorded in
   [`dependencies.md`](../docs/specs/dependencies.md) as a design reference with no code dependency, and
   a short credit goes in `THIRD_PARTY_NOTICES.md`.
3. **Catalogue by family.** The library is organized by these families, as Astryx does: Action,
   Container, Content, Feedback and Status, Form Controls, Layout, Navigation, Overlay, Table and
   List, Utility. The first release is the 28 foundational components, built in dependency order:

   | Tier | Components |
   |---|---|
   | 0 infrastructure | Theme scope, size context, Layer host (overlays and anchoring), VisuallyHidden, focus and Escape stack |
   | 1 primitives | Text, Heading, Icon, Stack, Divider, Spinner, Badge, StatusDot, Card, ScrollableArea, Link |
   | 2 overlay base | Tooltip, Popover |
   | 3 controls | Button, IconButton, Indicator, CheckboxInput, CheckboxList, RadioList, Switch, ProgressBar |
   | 4 field stack | FieldLabel, FieldStatus, Field, TextInput, TextArea |
   | 5 collections | Item, List, Selector, DropdownMenu |
   | 6 composites | Layout, Section, Dialog, AlertDialog, Toast, TabList, Table, Banner, EmptyState |

   Out of scope for v1: chat, charts, rich text and Markdown, date and time pickers, CommandPalette,
   touch-only sheets. ADR-047 item 7's desktop and tool controls (MenuBar, StatusBar, Splitter,
   PropertyGrid, docking) stay in the catalogue; they are not in Astryx and keep their stages.
4. **Anatomy and contract per component.** Each component has a spec under
   `docs/specs/components/<Name>.md` with: front matter (status, owners, verified-by test names), a
   contract table, intent, what it owns and does not own, public concepts (name, closed values,
   default, invalid-value behavior), numbered behavior rules (`FR`), accessibility rules (`AR`) and
   precedence rules (`ORD`), the states it has, and a verification map from each rule to a ctest or
   golden, with an explicit "evidence gap" when one is missing. A machine-readable block maps every
   named part to a **target**, `inherits`, `delegatesTo` or `none` with a reason. Tessera's spec
   template is one file, `docs/specs/components/_template.md`.
5. **Parts, states and variables are the styling API.** This sharpens ADR-047 item 3.
   - Each painted part has one target. A stylesheet reaches it with `::part(name)` (ADR-047) and the
     engine also sets stable attributes for each visual prop and state (`data-variant`, `data-size`,
     `aria-pressed`, `:disabled`, ...). A new attribute is public API and needs a spec rule.
   - A component may expose public CSS variables named `--<component>-*` (for example
     `--button-focus-offset`). Variables starting with `--_` are private and cannot be set.
   - A theme styles a component with keys: `base`, `variant:x`, `variant:x+size:y`, or a state name.
6. **Theme model.**
   - **Role tokens, one per role.** A colour role is one token, `--color-<property>-<role>`, whose
     value is `light-dark(L, D)`; there are never separate light and dark names. Groups: colour
     (background, text, icon, border, status with `-muted` and `on-` forms, data and syntax in their
     own layers), spacing (steps of 0.25rem), element size (28, 32, 36 px), radius tiers (inner,
     element, container, page, full), border width, focus (width, style, colour, offset), shadow
     (outer low, med, high; inset rings), motion (duration and easing tiers) and typography (family,
     size scale, weight, role triplets for size, weight and leading).
   - **A `defineTheme`-style source** (JSON or TSX; one format, chosen in the token spec) with
     `name`, `extends`, scale generators (type, radius, motion, accent colour), `tokens`, `components`
     and `adaptations`. `extends` resolves into one flat theme at build time. Explicit tokens beat
     generated ones.
   - **Adaptations** are ordered conditional overrides on width, pointer (coarse or fine), contrast
     and reduced motion; later rules win; they compile to media queries.
   - **A theme builds to plain CSS custom properties** inside `@layer`s. Only the generators run, at
     authoring time. Production code has no theme runtime. Nested theme regions (a dark sidebar in a
     light page) need `@scope` or a different mechanism: decision tracked in #622.
   - **Accessibility floors are part of the model:** text roles meet WCAG 2.2 AA, and a control edge
     meets 3:1 (Astryx documents `--color-border-control` but does not ship it; Tessera ships it).
   - **Theme versioning of ADR-047 item 4 is unchanged.** A pinned theme does not change look when
     components change.
   - Themes need engine features Tessera lacks: `light-dark()` (#620), `color-mix()` (#621), `:has()`
     and `@scope` (#622), inset shadows (#612). Until each lands, a theme uses the documented
     fallback (two token sets, plain mixes) and tests mark the gap by id.
7. **A behavior-hook layer (ADR-047 "primitives", now a catalogue).** Hooks are a contract, not a
   port. Each has a spec (name, arguments, the behavior it guarantees, accessibility) and is
   implemented once in the engine as a behavior class and reached from TSX through a compiler-level
   `behavior` hook kind (manifest, IR `behaviors`, generated wiring), designed in
   [`behavior-hooks.md`](../docs/design/behavior-hooks.md). Wrapper components are not an interim
   form: the existing primitives move onto the hooks. Astryx's React implementations are not
   reused. Staged as:
   - **Core (before the 28 components):** `useFocusTrap`, `useListFocus` (roving, one tab stop),
     `useTypeahead`, `useHotkeys`, `usePressFeedback`, `useAnnounce`, `useMergedRefs`,
     `useMediaQuery` (maps to `useBreakpoint` and media features), `useClipboard`, `useScrollLock`,
     `useOverflow`, `useInteractiveRole`.
   - **Needs an engine feature:** `useAnimator`, `useAnimatedLayout`, `useEntryAnimation` (builds on
     `useAnimatedValue` and `@starting-style` or an equivalent), `useContainerReveal`,
     `useScrollableArea`.
   - **After v1:** `useGridFocus`, `useTreeFocus`, `usePanGesture`, `useLongPress`,
     `useDragToSnapPoints`, `useStreamingText`, `useImageMode`, reorderable lists.
   - **Dropped:** browser-only helpers (dev warnings, IME and selection workarounds, click timing).
   - **Component-specific behavior** is a typed prop group of the component (the Table plugins
     below), not a hook that returns an opaque value. Package-declared hooks are not part of V1; a
     third-party package that needs its own behavior hooks is a later decision.
8. **Data components use a plugin model with caller-owned data.** `Table` (and later `DataGrid`)
   takes named plugins, each a typed prop group: `sorting`, `selection`, `pagination`, `infiniteScroll`,
   `filtering`, `grouping`, `tree`, `rowExpansion`, `columnResize`, `stickyColumns`, `virtualization`.
   Plugins run in a fixed canonical order, so their results do not depend on the order they are
   written in.
   - **Server data and lazy loading are plugins, and the application owns the data.** The component
     does not fetch. Infinite scroll takes `hasMore`, `isLoading` and `onLoadMore` (a function that
     returns when the batch is appended); pagination takes `page`, `onPageChange` and either
     `totalItems` or `totalPages` or `hasMore` (for cursor APIs with no total); sorting and filtering
     are controlled state the application sends to its server. The rows, cursors, errors and
     stale-response handling stay with the caller.
   - **Tessera adds one thing Astryx leaves to the caller:** a request token from the async runtime
     (`src/runtime/async.cpp` cancellation) is passed to `onLoadMore`, so a late response to an old
     query or sort is dropped by the framework, not by each application. A `DataSource` service
     (resolved with `useService`, ADR-040 and the app framework) is the intended place for the fetch.
   - **Rows are virtualized** by the `VirtualGrid` of
     [`virtualization-v1`](../docs/specs/virtualization-v1.md); loading and empty states are parts
     with the usual contract (a pending row never shifts layout).
   - This replaces the DataGrid scope of `component-library.md` section 8 ("virtual rows/columns,
     selection, sort, resize/reorder, keyboard") with a plugin list; the same features remain.
9. **Rules adopted as library invariants.** Semantic roles with contrast floors; one styling target
   per painted part; a disabled control stays focusable and explains why; a pending state never shifts
   layout; a composite control has one tab stop; logical properties for RTL; overlays and focus return
   follow one layer and focus stack; a11y is checked against data contracts, not by eye. ADR-063
   still governs defaults: the browser look unless a named exception.
10. **Staging and tracking.** The order is: tokens and theme model (with #620-#622), core hooks, tiers
    0 to 3, tier 4 (the first screen: a settings form), tiers 5 and 6 (menus, dialogs, tables). Each
    tier closes when its components meet ADR-047 item 10 (primitive tests, gallery page, parts,
    goldens per theme). Work is tracked under #618.

## Consequences

### Positive

- The library has a complete, ordered plan on day one instead of an invented one; the design cost is
  mostly copied structure, so production components arrive sooner.
- Theming is plain CSS custom properties: cheap to ship, easy to inspect, no runtime.
- Hooks become a reusable contract that components, applications and tests share; a missing hook
  shows up as a gap before a component hits it.
- Data components get server paging and lazy loading without a built-in data layer, and late
  responses are handled once in the framework.
- A tactile theme (the mock) becomes optional polish instead of a blocker.

### Negative / risks

- Several engine features are now prerequisites (`light-dark()`, `color-mix()`, `:has()`/`@scope`,
  inset shadows, anchoring and top layer for overlays); until they land, themes use fallbacks.
- Astryx can change or disappear; we record the reviewed commit and keep our own specs, so we drift
  instead of break.
- A large hook list is a large API surface; the "core" subset and the staging keep it small, and each
  hook needs an owner and a test per rule.
- The resemblance is structural, so naming must stay ours where our model differs (for example no
  runtime theme object, `::part()` instead of generated classes).

## Alternatives considered

- **Keep Instrument as the default and add the catalogue later.** Rejected: the look blocks nothing
  useful while the catalogue and theme model block everything.
- **Port Astryx's components as code.** Rejected: React and StyleX, and Tessera has no JS runtime;
  the TSX subset is smaller.
- **Adopt another system (Material, Fluent, Radix) as the model.** Rejected for now: Astryx is MIT,
  documents contracts per component and its theme output is plain CSS; ADR-047's reasons against
  taking another vendor's look still apply to the default theme.
- **Make data fetching part of the table.** Rejected: server shapes differ; a controlled plugin with
  a request token covers cursor and total-count APIs without an adapter zoo.

## Evidence required before Accepted

This ADR stays Proposed until measured evidence exists. Spike [#658](https://github.com/thnak/Tessera/issues/658) builds the behavior hook path end to end with one behavior (`press_feedback`), off by default, and records the stages' findings, allocation behavior, golden stability and preview support. Spike [#662](https://github.com/thnak/Tessera/issues/662) builds a role-token theme to plain CSS custom properties and renders Button in three modes, recording missing engine features, measured contrast and the cost of the token vocabulary. Items 3 to 5 and 8 (catalogue, anatomy, plugins) have no spike yet; they are specifications that the first components (#636 Button) will test. The implementing issues wait for Acceptance.

### Spike #662 evidence (item 6, recorded 2026-11-21)

Built off by default: `include/tessera/ui/style/theme_source.hpp`,
`src/ui/style/theme_source_{parse,build}.cpp`, test `theme_source`, fixture
`tests/fixtures/themes/spike-instrument.theme.json`. Normative rules:
[`theme-source-v1.md`](../docs/specs/theme-source-v1.md). Nothing runs in a shipped app; the build is
an authoring-time library call.

**The model holds.** A role token is one name whose value is a `light-dark()` pair, and the engine's
own cascade resolves it: the same built CSS, compiled by `prepare_stylesheet` and resolved through
`SceneStyleTree::compute`, gives the Button its accent fill in the light scheme and the dark fill in
the dark scheme from the same token name. There are no separate light and dark names anywhere in the
output. Everything lands in one `@layer tessera-ui.theme` block, so unlayered app CSS outranks it.

**What the engine was missing, and how the fallback looked.** Three gaps, none of them blocking:

1. **`prefers-contrast` is not a media feature.** The engine has `prefers-color-scheme`,
   `prefers-reduced-motion`, the viewport features and `window-backdrop`, but no contrast query, so a
   high-contrast variant is a second theme id today (the shipped `instrument-high-contrast`) rather
   than a media query over the same token names. The spike measures high contrast by resolving the
   same token set with the stricter floors; the fallback is a theme switch, which is what ships.
2. **`light-dark()` is a colour function only.** A length, a font stack or a number cannot take a
   pair, so the build emits those tokens once. That is correct CSS, but it means a theme source has
   two token shapes (a string and a `[light, dark]` pair) rather than one, and the build decides
   which by parsing the value with the engine's own `parse_color` so the two cannot disagree.
3. **`adaptations` has no source syntax.** The engine's media evaluation can express the four
   conditions ADR-064 item 6 names; what is missing is the source-side syntax and the ordering rule
   ("later rules win"). The spike accepts the member and ignores it, and records the gap here rather
   than inventing a syntax the token spec has not decided.

**Measured contrast.** The fixture's Button ink on its fill clears the normal-text floor in all three
modes (4.5:1 light and dark, 7:1 high contrast), measured with `contrast_ratio` over the resolved
colours rather than over the authored token values. The secondary variant's fill is the raised
surface, a subtle panel and not the control's boundary, so the 3:1 non-text floor applies to
`--color-border-control` instead; the spike records that distinction rather than asserting a floor
the variant does not claim.

**The cost of the token vocabulary.** The fixture defines 20 tokens; Button reads 15 of them. The
five it does not read are the page surface, the muted text role, the compact spacing, the compact
control height and the focus ring's own value — they belong to other components or to app CSS. The
build reports both directions (`TESSERA_THEME_SOURCE_UNUSED_TOKEN`,
`TESSERA_THEME_SOURCE_UNKNOWN_TOKEN`) as warnings, so the ratio is a number a token spec can keep in
view rather than a guess. A per-role vocabulary is the right shape; a per-component one would define
the same colour four times.

**What this does not settle.** Items 3 to 5 and 8 of this ADR (catalogue, anatomy, plugins) are
untouched, and the token spec itself is still a follow-up: the spike fixes the *build*, not the
vocabulary. The `adaptations` syntax and a `tessera theme build` subcommand are the two concrete
next steps if item 6 is Accepted.


### Spike #658 evidence (item 7, recorded 2026-11-21)

Built off by default: the engine seam (`include/tessera/ui/behavior/behavior.hpp`), the registry
(`src/ui/behavior/registry.cpp`), `press_feedback` (`src/ui/behavior/press_feedback.cpp`), the
shared wiring class (`include/tessera/ui/behavior/instance.hpp`), the analyzer and lowering
(`src/compiler/rules/src/hooks.cpp`, `src/compiler/lowering/src/binding_lowering.cpp`), the IR
`behaviors` list, the generated base's behavior surface (`src/compiler/codegen/src/emit_behaviors.cpp`)
and the preview wiring (`tools/codegen/scene_session_behavior.cpp`). Normative rules:
[`behavior-hooks-v1.md`](../docs/specs/behavior-hooks-v1.md). Nothing runs in a shipped app unless a
component calls a behavior hook, and no committed `.g.hpp` golden moved: a component without
behaviors generates the bytes it generated before.

**The model holds, and the seam is the right one.** A behavior is engine code, not generated code:
the generated base and the preview session each own one `behavior::Instance` and call the same four
methods, so neither implements a behavior twice. `press_feedback` reads the scene's own
`InputRouter` state rather than registering listeners, which is what makes it a pure function of the
interaction state and a `ManualClock` test deterministic.

**Three defects the spike found, all of which made the path unreachable.** These are the reason the
spike was worth building: each looked like a different bug, and none was visible from a green build.

1. **The lowering dropped the behaviors.** `node_lowering.cpp` assigned `state`, `handlers`,
   `effects`, `refs` and `animated_values` onto the IR component and never assigned `behaviors`. The
   analyzer classified the call and the lowering built the entry, and then it was discarded — so no
   component ever carried a behavior and every downstream consumer was dead code no test could
   reach. The existing `lowering_behaviors` test passed because it asserted on the *lowering result*
   rather than on the component the lowering produced.
2. **A behavior output had no reference form.** `ComponentNames` resolved props, state, refs and
   animated values but not an output, so `press.pressed` failed with `TESSERA_IR_UNRESOLVED`. The
   spec already stated the rule (section 6: `beh::<name>.<field>`); the resolver simply did not
   implement it. Only a field the hook actually returns resolves, so `press.nope` still reports
   rather than the generator inventing an accessor.
3. **The evaluator's scope had no `beh::` entries.** The first build failed on an unknown ref. The
   spec's section 8 rule — "before the first attach an output holds its manifest default" — is what
   fixes it: `build_scene` seeds each output with that default and the session overwrites it from the
   live instance after it attaches.

**What the spec got wrong, and how it was amended.** The spec's section 9 rule "call `update` only
when the hash of the evaluated value options changed" is correct for a behavior whose outputs are a
function of its options, and **wrong for one whose outputs are a function of interaction state**.
`press_feedback` has no options at all, so its hash never changes and a hash-only update would skip
it forever: the Button would report "not pressed" through an entire press. The seam therefore has
two triggers, and the distinction is now in the spec:

- `update(args)` — the component's own state changed, so the evaluated options may have changed.
- `refresh(args)` — the scene moved on (a pointer moved, a press began, focus moved) while the
  component's options did not change at all.

`refresh` deliberately does not touch the stored hash, so a scene-driven refresh cannot defeat the
option-driven skip. Both directions are pinned by `behavior_instance`.

**Allocation.** `attach` allocates the engine behavior through the registry factory — one allocation,
the slot the generated base owns. `update`, `refresh` and `detach` allocate nothing: the hash is a
value and the args are a borrowed view. `behavior_allocation` pins this by counting global
`operator new`, and `behavior_instance` pins that the factory allocates exactly once.

**Golden stability.** No committed `.g.hpp` golden moved. Emission is conditional on
`component.behaviors` being non-empty, so a component that calls no behavior hook generates no
behavior code at all — which is every component in the repository today.

**The preview drives the same instance.** `scene_behavior` proves it: a component calling
`usePressFeedback` produces one behavior in the session, named from the registry, attached to the
node its `ref` resolved to, and a pointer press dispatched through the session's own `dispatch()`
moves the outputs. This is the evidence section 10 asked for, and it is the same `Instance` the
generated base uses rather than a second implementation.

**What this does not settle.** Items 3 to 5 and 8 of this ADR (catalogue, anatomy, plugins) are
untouched. The remaining core behaviors (`focus_trap`, `roving_focus`, `typeahead`, `hotkeys`,
`scroll_lock`, `overflow`, `announce`, `clipboard`, `interactive_role`) are one issue each and are
not part of this spike; `focus_trap` is the next one, because Dialog needs it. The preview does not
yet resolve a callback arg to a thunk (it records the handler id and passes 0), so a behavior that
invokes a callback is wired but inert in the preview — a gap to close with the first behavior that
needs one.


## Follow-up

- Spec template `docs/specs/components/_template.md`; first specs: Button, Field, TextInput, Dialog,
  TabList, Table.
- Token spec v1 (role tokens, `light-dark()` pairs, `extends`, adaptations) and a token-build tool;
  update `component-library.md` and `stylesheet-v1.md`.
- The `behavior` hook kind first (design in `behavior-hooks.md`, then a spec, analyzer and lowering,
  engine interface and registry, code generation, preview wiring), then one issue per core hook
  (milestone V1-M4) and the stage-2 hooks as their engine features land.
- Table plugin contract with the request token; revise the DataGrid scope in
  `component-library.md`.
- Move the tactile mock under `samples/` as a demo ("Device" theme candidate) and update its README.
- Record the Astryx reference commit in `dependencies.md` and `THIRD_PARTY_NOTICES.md`.
