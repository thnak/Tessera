# Accessibility v1

Status: proposed (Big Step F). Decision: ADR-030. This is the normative contract; design
rationale is in [`accessibility.md`](../design/accessibility.md). Companion to
[`text-editing-v1.md`](text-editing-v1.md), [`overlays-and-layers-v1.md`](overlays-and-layers-v1.md),
[`navigation-v1.md`](navigation-v1.md), and [`mobile-integration.md`](../design/mobile-integration.md).

Accessibility is a first-class tree parallel to the scene graph, not a post-processing step.

## 1. Semantics model

```cpp
struct SemanticsNode {
    NodeId node;
    Role role;
    std::string name;             // required for interactive nodes
    std::string description;
    std::string value;
    State state;                  // focused/disabled/checked/selected/expanded/busy/invalid...
    Actions actions;              // click/focus/set_value/increment/scroll/dismiss/show_menu
    Rect bounds;                  // window/screen logical pixels
    std::vector<NodeId> children;
    std::string error;            // associated error text, optional
};
```

- Every interactive node has role + name + actions; missing names are test failures.
- Decorative nodes are excluded; containers may be groups.
- Semantics updates on semantic/layout/text/focus/selection changes, not every frame.
- The tree is UI-thread-owned; platform bridges read immutable snapshots.
- Hidden/invisible/disabled states are explicit; no ghost focus targets.

## 2. Roles, states, actions

- Roles: generic, button, link, text, heading, image, text field, checkbox, radio, switch,
  slider, progress, list, list item, tab, tab panel, dialog, menu, menu item, scroll view,
  tooltip, alert.
- States: focused, focusable, disabled, checked/mixed, selected, expanded/collapsed, busy,
  invalid, read-only, required.
- Actions: activate/click, focus, set value, increment/decrement, scroll, dismiss, show menu,
  select text, set selection, copy/cut/paste.
- Unknown roles degrade to generic with a diagnostic; never drop the node silently.

## 3. Focus and keyboard

- Focus order follows the semantics tree; `tab_index` can override.
- Focus is visible; `:focus-visible` for keyboard/accessibility focus.
- Modal focus trap and restore; escape/back has an explicit path.
- Screen-reader focus and keyboard focus synchronize; avoid double announcements.
- Arrow keys navigate lists/tabs/grids; Enter/Space activate; Escape dismisses.
- Shortcuts do not fire during IME composition unless explicitly allowed.
- Focused item is scrolled into view and realized (virtualized/custom).

## 4. Text editing accessibility

- Expose text value, selection range, caret position, placeholder, read-only/disabled.
- Support set value/select/copy/cut/paste actions; secure fields redact value.
- Errors/help/required are associated and announced once.
- Composition/marked text is exposed where the platform supports it.
- Caret/selection rectangles come from shaped layout; bidi visual order is respected.
- Announce selection changes at a bounded rate.

## 5. Overlays, navigation, and live regions

- Dialog role + name + initial focus + focus trap + restore.
- Menu/menuitem roles, expanded/checked states, keyboard navigation.
- Tooltips are associated with the trigger and are not the only information source.
- Route/screen changes announce the new screen title/context; focus moves to a sensible node.
- Toasts/status use live regions; coalesce to avoid screen-reader flooding.
- Background is hidden from the semantics tree while a modal is open.

## 6. Virtualized and custom elements

- Virtualized list items expose only realized nodes; the list exposes item count/index.
- Focused/selected item is realized before focus/announcement.
- Custom elements declare semantics in `ui/elements.json` and may update via
  `on_semantics(SemanticsBuilder&)`; 3D/custom rendering needs a meaningful fallback.
- `partId` may refine custom sub-targets; the semantics node still has a stable role/name.
- See [`virtualization-v1.md`](virtualization-v1.md) and
  [`custom-elements-v1.md`](custom-elements-v1.md).

## 7. Platform bridges

| Platform | API | Notes |
|---|---|---|
| Windows | UI Automation (UIA) | providers, fragments, focus/selection/scroll patterns |
| Linux | AT-SPI over D-Bus | no libatspi/glib linkage (ADR-012) |
| macOS | NSAccessibility | later macOS port |
| iOS/iPadOS | UIAccessibility | later mobile target |
| Android | AccessibilityNodeInfo/ExploreByTouchHelper | JNI at OS edge |

- Bridges are optional at runtime; if unavailable, the app runs with accessibility disabled
  and a diagnostic.
- No UI objects cross the bridge; pass values/snapshots/handles.
- Bridge calls happen on the platform thread, not in the render/input hot path.
- Action callbacks map to UI-thread commands; no re-entrancy.

## 8. Announcements and errors

```cpp
announce(message, priority);   // polite/assertive
```

- Errors are announced and associated with the field; summaries link to fields.
- Busy/loading announcements are debounced; avoid announcing every progress tick.
- Do not announce sensitive/secure text.
- Localized messages use `i18n`; no raw internal codes.

## 9. Testing

- Semantics snapshots: role/name/state/actions/bounds/children.
- Focus order, trap/restore, keyboard navigation, screen-reader focus.
- Text editing selection/caret/error/secure redaction.
- Overlay/menu/tooltip/toast roles and announcements.
- Virtualized/custom element semantics and realization.
- Text scale/high contrast/reduced motion layout goldens.
- Platform bridge tests with real screen readers (Windows/NVDA, Linux/Orca; mobile later).
- Missing names/roles/actions are CI failures.

## 10. Performance and security

- Semantics snapshots are bounded and updated on dirty, not per frame.
- Platform bridge payloads are size-limited; no unbounded trees.
- Redact values for secure fields; no secrets/PII in snapshots/diagnostics.
- Accessibility actions validate node ids/permissions; no arbitrary action execution.

## 11. Open questions

- Exact role mapping per platform and fallback behavior.
- Announcement deduplication across bridges.
- Semantics for 3D/custom elements and charts.
- Multi-window/global focus.
- High-contrast token coverage.
- Screen-reader test automation in CI.
