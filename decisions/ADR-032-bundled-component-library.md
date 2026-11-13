# ADR-032: Bundled component library and design tokens

- Status: Accepted; items 1, 2, 3 and 6 amended by [ADR-047](ADR-047-component-library-layers-and-design-language.md) (2026-11-11)
- Date: 2026-11-07
- Related: [ADR-029](ADR-029-interaction-completeness.md) (interaction completeness), [ADR-030](ADR-030-app-platform-completeness.md) (app-platform completeness), [ADR-031](ADR-031-production-tooling-and-distribution.md) (production tooling and distribution)

## Context

Framework primitives alone force every team to rebuild buttons, inputs, dialogs, menus, and data
grids, which causes inconsistent accessibility and visual behavior. A bundled component library
is a product layer, not part of the renderer.

## Decision

1. Ship a separate `tessera-ui` package built only on public APIs; source + generated C++, no
   opaque binaries or JS runtime.
2. Design tokens (color/type/spacing/radius/elevation/motion/breakpoints/z-layers) with
   semantic themes: light/dark/high-contrast, density, RTL.
3. Provide accessible primitives: actions, inputs, layout, navigation, overlays, data/table,
   feedback, utilities. DataGrid builds on virtualization; overlays on the overlay service.
4. Every component documents role/name/state/actions, keyboard/focus, disabled/read-only,
   touch target, text scale, high contrast, reduced motion, loading/empty/error.
5. Test with semantics snapshots, keyboard/pointer/IME/overlay/headless scripts, and
   theme/density/RTL/text-scale [goldens](../docs/reference/glossary.md). Visual changes require reviewed goldens.
6. Package SemVer (semantic versioning); unused components tree-shake; fonts/icons/licenses documented.

## Consequences

- Faster adoption and consistent accessibility.
- Maintenance cost and versioning of visual behavior.
- Risk of the library becoming a de-facto core; keep it optional and replaceable.
- DataGrid/DatePicker/Charts are large scopes; the Follow-up list delivers them in stages (DataGrid/DatePicker/Tree after the basic components).

## Alternatives

- No library, examples only: rejected (fragmentation).
- Third-party widget library: rejected (accessibility/theming/native integration mismatch).
- Web components/HTML widgets: rejected (bridge/JS/native-first).

## Follow-up

- Tokens + Button/Text/Input/Checkbox/Radio/Switch.
- Dialog/Menu/Popover/Tooltip/Toast.
- Tabs/Sidebar/AppBar/List/Table.
- DataGrid/DatePicker/Tree; charts/3D via custom elements.
- Publish docs/examples and golden baselines.

## Release scope (added 2026-11-08)

The bundled component library, including the initial DataGrid, ships after V1. V1 is defined in
the [glossary](../docs/reference/glossary.md). This note adds the release decision that the
staging above did not make; it does not change the staging.

## Amendment: stages 0 and 1 ship in V1 (2026-11-13)

The owner moved the first two stages of the staging in [`component-library.md`](../docs/design/component-library.md)
section 8 into V1: stage 0 (foundations: package imports, `::part()`, behavior primitives, tokens
and the Instrument theme, Icon with the Iconify import tool, gallery skeleton) and stage 1 (a
settings screen: core controls, inputs, layout, feedback, SafeArea, Image, List, Tooltip).
Reason: without controls V1 would give developers no accessible way to build an ordinary screen.
Stages 2 to 6, including the initial DataGrid, stay after V1. V1 targets Windows and Linux.
