# Accessibility

Status: design for [Big Step F](../../AGENTS.md#big-steps-group-work-dont-micro-step) (production breadth; Windows/Linux first). `src/ui/accessibility/` is a placeholder.
Normative roles/actions/announcements/bridges/tests: [`accessibility-v1.md`](../specs/accessibility-v1.md).

Accessibility is part of the UI model ([`ui-model.md`](ui-model.md)), not an afterthought: every interactive node owns
semantics, focus, and actions. The rendering path must also respect text scale, high contrast,
and reduced motion.

## 1. Semantics tree

A parallel tree mirrors the scene graph ([`scene-graph.md`](scene-graph.md)):

```cpp
struct SemanticsNode {
    NodeId node;
    Role role;                    // button, text, text_field, checkbox, slider, ...
    std::string name;             // accessible name
    std::string description;      // help text
    std::string value;            // current value (slider/text field)
    State state;                  // focused, disabled, checked, selected, expanded, busy
    Actions actions;              // click, focus, set_value, increment, ...
    Rect bounds;                  // screen/window coordinates
    std::vector<NodeId> children;
};
```

- Built from explicit component semantics (preferred) and defaults per node kind (the default role table is not specified).
- Updates on semantic dirty (invalidation, see [`dirty-rendering.md`](dirty-rendering.md)), layout bounds change, text/value change, and focus changes.
- Hidden/invisible nodes are removed; `accessibilityElementsHidden` (iOS)/`importantForAccessibility` (Android)
  equivalents are supported.
- The tree is UI-thread-owned; platform bridges read immutable snapshots.

## 2. Roles and states

V1 roles (first-release scope, see [AGENTS.md rule 8](../../AGENTS.md#non-negotiables)): generic, button, link, text, heading, image, text field, checkbox, radio, switch,
slider, progress, list, list item, tab, tab panel, dialog, menu, menu item, scroll view.

States: focused, focusable, disabled, checked/mixed, selected, expanded/collapsed, busy,
invalid, readonly, required.

Actions: activate/click, focus, set value, increment/decrement, scroll, dismiss, show menu.

Map roles/actions to platform APIs; unknown roles degrade to generic with a diagnostic.

## 3. Platform bridges

| Platform | API | Notes |
|---|---|---|
| Windows | UI Automation (UIA) via COM (Component Object Model) | provider interfaces, fragments, focus/selection patterns |
| Linux | AT-SPI (Assistive Technology Service Provider Interface) over D-Bus | do **not** link libatspi/glib (LGPL; [ADR-012](../../decisions/ADR-012-static-linking.md), static linking); use the protocol/system service |
| macOS | NSAccessibility | deferred with the macOS port ([ADR-010](../../decisions/ADR-010-macos-via-moltenvk-deferred.md)) |
| iOS/iPadOS | UIAccessibility | Big Step F, after Android ([`mobile-integration.md`](mobile-integration.md)) |
| Android | AccessibilityNodeInfo / ExploreByTouchHelper via JNI (Java Native Interface) | Big Step F, first mobile target ([`mobile-integration.md`](mobile-integration.md)) |

Bridges are optional at runtime: if the platform service is unavailable, the app still runs;
accessibility is disabled with a diagnostic. Mobile-specific semantics/IME/declarations are in
[`mobile-integration.md`](mobile-integration.md).

## 4. Focus and keyboard

- Focus order follows the scene/semantics tree by default; explicit `tabindex`-like ordering
  can override.
- Focus is visible (`:focus-visible`) and never trapped without an escape path.
- Dialogs/modals trap focus and restore it on close.
- Arrow keys navigate lists/tabs/grids; Enter/Space activate; Escape dismisses.
- Keyboard shortcuts are global or scoped; they do not fire while an IME (input method editor) composition is active
  unless the shortcut was registered with `allowDuringComposition = true` (default `false`; see
  [`accessibility-v1.md`](../specs/accessibility-v1.md)).
- Screen-reader focus and keyboard focus are synchronized; avoid double announcements.

## 5. Text scale, contrast, motion

- Text scale/font weight reflows layout; no clipping/overlap.
- High contrast: use system colors/contrast tokens; do not rely on subtle color differences.
- Reduced motion: skip non-essential transitions/springs; keep functional state changes.
- Bold text/invert colors (iOS) and accessibility display options (macOS) are inputs.
- Touch targets remain at least 44/48 dp (density-independent pixels) after scaling; behavior when scaling makes this impossible is not specified.
- Focus rings/hit targets must remain visible in high contrast.

## 5.1 Custom elements

- `ui/elements.json` declares default semantics (role/name/description); the element may
  update value/state/actions/children via `on_semantics(SemanticsBuilder&)`.
- A 3D/custom-rendered element must provide a meaningful non-visual fallback: name,
  description, keyboard actions, focus, and state. Canvas-only interaction is not acceptable.
- Custom elements participate in the normal semantics tree, focus order, and actions; snapshots
  are test artifacts. See [`custom-elements-v1.md`](../specs/custom-elements-v1.md).

## 6. Announcements and live regions

- `announce(message, priority)` for status updates (async completion, validation).
- Live regions for streaming data; coalesce to avoid screen-reader flooding.
- Do not announce every chart tick; announce summaries at a bounded rate.
- Errors are announced and associated with the field (name/description/error text).

## 7. Testing

- Semantics snapshots: role/name/state/actions/bounds for a scene.
- Focus order and modal trap/restore tests.
- Text scale/high contrast layout goldens.
- Platform bridge unit tests with fake providers.
- Manual screen-reader matrix: Windows Narrator/NVDA, Linux Orca, macOS VoiceOver (with the macOS port, Big Step F).
- Keyboard-only navigation tests.
- No accessibility tree update on every frame; verify dirty-only updates.

## 8. Performance

- Semantics updates are coalesced at frame/tick boundaries.
- Platform calls are batched; never block the UI thread on an accessibility service.
- The tree is snapshot-based; platform bridges do not hold scene pointers.
- Disabled accessibility adds no measurable per-frame cost.

## 9. Open questions

- How much semantics is inferred vs explicitly authored.
- AT-SPI over D-Bus without linking LGPL libraries: exact service/protocol subset.
- UIA provider threading and COM (Component Object Model) apartment rules.
- Screen-reader behavior for virtualized lists/charts; see
  [`virtualization-v1.md`](../specs/virtualization-v1.md).
- Accessibility for custom-drawn text/paths.
