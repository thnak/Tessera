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

DataGrid scope (V1 library):

- Virtualized rows/columns, selection, sorting, column resize/reorder, keyboard navigation.
- Editing/grouping/pivot are deferred; do not block the core on them.
- Headless mode exposes rows/cells for tests; semantic table/grid roles.

Testing contract:

- API/state tests, keyboard/focus, semantics snapshot, theme/density/RTL/text-scale goldens.
- No component ships without an accessibility contract and a headless interaction test.
- Visual changes require reviewed goldens; semantic token changes are versioned.

Gap-closure decisions:

- **G-25/Q-24**: token JSON v1 is semantic and SemVer-versioned; component API contract includes
  controlled state, focus, and accessibility; DataGrid V1 supports virtual rows/columns,
  selection, sort, resize/reorder, keyboard. Editing/grouping/pivot are deferred.
- Components ship styled by default with a headless escape hatch for custom rendering; fonts are
  app assets unless the app opts into the bundled font pack.

## 9. Deferred/open questions

- Icon set/format and licensing.
- DataGrid editing/grouping/pivot.
- Bundled font pack/versioning.
- Token rename migration tooling.
- Charts/3D component wrappers.
