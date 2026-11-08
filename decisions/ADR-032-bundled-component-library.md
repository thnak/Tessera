# ADR-032: Bundled component library and design tokens

- Status: Accepted
- Date: 2026-11-07
- Related: ADR-029, ADR-030, ADR-031

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
   theme/density/RTL/text-scale goldens. Visual changes require reviewed goldens.
6. Package SemVer; unused components tree-shake; fonts/icons/licenses documented.

## Consequences

- Faster adoption and consistent accessibility.
- Maintenance cost and versioning of visual behavior.
- Risk of the library becoming a de-facto core; keep it optional and replaceable.
- DataGrid/DatePicker/Charts are large scopes; stage them.

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
