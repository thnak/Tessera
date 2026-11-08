# Component library and design system

Status: design (post-V1 product layer). Decision: ADR-032.

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
- Token changes are style-only; no component recompilation needed where possible.
- Tokens are versioned; visual changes require golden updates.

## 3. Primitives

| Group | Components |
|---|---|
| Actions | Button, IconButton, ToggleButton, Link |
| Inputs | TextInput, TextArea, Checkbox, Radio, Switch, Slider, Select, DatePicker |
| Layout | Stack, Grid, Card, Divider, Spacer, ScrollView |
| Navigation | Tabs, Breadcrumb, Sidebar, AppBar, Pagination |
| Overlays | Dialog, Sheet, Popover, Tooltip, Menu, ContextMenu, Toast |
| Data | List, Table/DataGrid, Tree, VirtualList/Grid, EmptyState, Skeleton |
| Feedback | Progress, Spinner, Banner, Alert, Badge |
| Utilities | Avatar, Chip, Accordion, Tooltip, VisuallyHidden |

- DataGrid builds on `VirtualGrid`, selection, sorting, column resize/reorder, keyboard nav.
- Forms use `forms-and-validation-v1`; overlays use `overlays-and-layers-v1`.
- 3D/chart components use custom elements, not core changes.

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
- RTL/bidi-aware; icons mirror only when semantic.
- Platform conventions (keyboard shortcuts, menu order) are overridable.

## 6. Testing

- Unit: state/interaction/keyboard/focus/validation.
- Semantics snapshots + accessibility checks (names/roles/actions).
- Golden: default/dark/high-contrast, compact/expanded, RTL, text scale.
- Headless: keyboard/pointer/IME/overlay/data-grid scripts.
- Visual regression across themes/densities; no real GPU baselines.

## 7. Packaging

- `tessera-ui` package with source, generated code, tokens, docs, examples.
- SemVer; visual breaking changes documented with migration/golden updates.
- Tree-shake at build time; unused components not linked.
- Licenses/fonts/icons packaged and documented.

## 8. Roadmap

1. Tokens + Button/Text/Input/Checkbox/Radio/Switch.
2. Dialog/Menu/Popover/Tooltip/Toast.
3. Tabs/Sidebar/AppBar/List/Table.
4. DataGrid/DatePicker/Tree.
5. Charts/3D via custom elements.

## 9. Open questions

- Icon set/format and licensing.
- Headless vs styled component split.
- DataGrid feature scope (editing, grouping, pivot).
- Whether to bundle fonts or require app assets.
- Versioning of token names.
