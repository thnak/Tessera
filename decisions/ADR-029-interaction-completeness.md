# ADR-029: Interaction completeness: text editing, scrolling, overlays, forms

- Status: Accepted
- Date: 2026-11-07
- Related: ADR-007, ADR-017, ADR-018, ADR-025

## Context

The framework defines rendering, scene graph, input, virtualization, and error boundaries, but
real applications also need text editing, scroll/gesture arbitration, layers above the base tree,
and form validation. Without these, every app reinvents fragile widgets.

## Decision

1. **Text editing** is a core primitive: `TextInput`/`TextArea`, grapheme/bidi selection,
   caret/composition, IME, undo/redo, clipboard, secure fields, accessibility, deterministic
   tests. Text/plain is V1; rich text is later.
2. **Scroll/gesture** is one system: `Scroll` container, physics, nested scroll/chaining,
   snap/paging, scrollbars/anchoring, and a gesture arena (tap/drag/pinch/rotate) on the
   normalized pointer stream.
3. **Overlays** replace portals: `OverlayHost` + `OverlayService` with explicit layers
   (overlay/modal/tooltip/toast), anchoring/flip/shift/clamp, focus trap/restore, dismiss/back,
   and accessibility. No arbitrary z-index/portals.
4. **Forms** are a library/runtime contract: typed field state, dirty/touched, sync/async
   validation with stale-epoch cancellation, submit/focus-first-invalid, reset, autofill, and
   accessibility.
5. All four use normal input/layout/paint/style paths, numeric automation IDs, headless/golden
   tests, and no JS runtime/dynamic plugins.

## Consequences

- Real apps can be built without ad-hoc widget stacks.
- More surface area in UI runtime and component library.
- Text/IME/gesture behavior must be platform-tested, not just golden-tested.
- Overlay focus/back interactions need careful integration with navigation.
- Forms validation must integrate i18n/accessibility.

## Alternatives

- Leave editing/scroll/overlays/forms to apps: rejected (duplication/inconsistency).
- Use portals/DOM-style z-index: rejected (native scene graph/accessibility).
- Browser contenteditable/WebView for text: rejected (bridge/JS/native-first).
- One giant form/input component: rejected (composition/accessibility).

## Follow-up

- Implement text editing model/commands/IME/undo and nodes.
- Implement scroll physics/gesture arena and nested scroll.
- Implement overlay host/service/focus/accessibility.
- Implement forms library on text inputs; add tests/goldens/headless scripts.
