# Accessibility v1

Status: proposed (Big Step F, defined in [AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step)). "V1" means the first-release scope ([AGENTS.md](../../AGENTS.md), Non-negotiables rule 8). Decision: [ADR-030](../../decisions/ADR-030-app-platform-completeness.md) (navigation, restore, shell, i18n, accessibility, async data). This is the normative contract; design
rationale is in [`accessibility.md`](../design/accessibility.md). Companion to
[`text-editing-v1.md`](text-editing-v1.md), [`overlays-and-layers-v1.md`](overlays-and-layers-v1.md),
[`navigation-v1.md`](navigation-v1.md), and [`mobile-integration.md`](../design/mobile-integration.md).
The deterministic semantics snapshot builder (roles, names, hidden rules, focus, actions, bounds)
is implemented; see section 12.

Accessibility is a first-class tree parallel to the scene graph ([`scene-graph.md`](../design/scene-graph.md)), not a post-processing step. `NodeId` is the runtime scene-node id defined in [`automation-ids-v1.md`](automation-ids-v1.md) section 1.

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
- Shortcuts do not fire during IME (input method editor) composition unless a shortcut declares `allowDuringComposition` (boolean, default `false`) when it is registered; the registration API itself is not specified, see [`input.md`](../design/input.md) ([`accessibility.md`](../design/accessibility.md)).
- Focused item is scrolled into view and realized (virtualized lists, [`virtualization-v1.md`](virtualization-v1.md), or custom elements, [`custom-elements-v1.md`](custom-elements-v1.md)).

## 4. Text editing accessibility

- Expose text value, selection range, caret position, placeholder, read-only/disabled.
- Support set value/select/copy/cut/paste actions; secure fields redact value.
- Errors/help/required are associated and announced once.
- Composition/marked text is exposed on platforms whose accessibility API supports it (the per-platform list is not specified).
- Caret/selection rectangles come from shaped layout; bidi (bidirectional text) visual order is respected.
- Announce selection changes at a bounded rate.

## 5. Overlays, navigation, and live regions

- Dialog role + name + initial focus + focus trap + restore.
- Menu/menuitem roles, expanded/checked states, keyboard navigation.
- Tooltips are associated with the trigger and are not the only information source.
- Route/screen changes announce the new screen title/context; focus moves to the heading or first control (section 10.1).
- Toasts/status use live regions; coalesce to avoid screen-reader flooding.
- Background is hidden from the semantics tree while a modal is open.

## 6. Virtualized and custom elements

- Virtualized list items expose only realized nodes; the list exposes item count/index.
- Focused/selected item is realized before focus/announcement.
- Custom elements declare semantics in `ui/elements.json` and may update via
  `on_semantics(SemanticsBuilder&)`; 3D/custom rendering needs a meaningful fallback.
- `partId` (a per-element sub-target id, [`automation-ids-v1.md`](automation-ids-v1.md) section 5.1) may refine custom sub-targets; the semantics node still has a stable role/name.
- See [`virtualization-v1.md`](virtualization-v1.md) and
  [`custom-elements-v1.md`](custom-elements-v1.md).

## 7. Platform bridges

| Platform | API | Notes |
|---|---|---|
| Windows | UI Automation (UIA) | providers, fragments, focus/selection/scroll patterns |
| Linux | AT-SPI (Assistive Technology Service Provider Interface) over D-Bus | no libatspi/glib linkage ([ADR-012](../../decisions/ADR-012-static-linking.md), static linking) |
| macOS | NSAccessibility | macOS port (deferred from V1, [ADR-010](../../decisions/ADR-010-macos-via-moltenvk-deferred.md)) |
| iOS/iPadOS | UIAccessibility | iOS comes after Android (Big Step F); no release assigned |
| Android | AccessibilityNodeInfo/ExploreByTouchHelper | JNI (Java Native Interface) at the OS edge |

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
- Localized messages use `i18n` ([`i18n-v1.md`](i18n-v1.md)); no raw internal codes.

## 9. Testing

- Semantics snapshots: role/name/state/actions/bounds/children.
- Focus order, trap/restore, keyboard navigation, screen-reader focus.
- Text editing selection/caret/error/secure redaction.
- Overlay/menu/tooltip/toast roles and announcements.
- Virtualized/custom element semantics and realization.
- Text scale/high contrast/reduced motion layout goldens (reference images, [`golden-tests-v1.md`](golden-tests-v1.md)).
- Platform bridge tests with real screen readers (Windows/NVDA, Linux/Orca, both screen readers; mobile is deferred, see section 11).
- Missing names/roles/actions are CI failures.

## 10. Performance and security

- Semantics snapshots are bounded and updated on dirty flags ([`dirty-rendering.md`](../design/dirty-rendering.md)), not per frame.
- Platform bridge payloads are size-limited; no unbounded trees.
- Redact values for secure fields; no secrets/PII in snapshots/diagnostics.
- Accessibility actions validate node ids/permissions; no arbitrary action execution.

## 10.1 Detail design

Ownership:

| Component | Owns | Does not own |
|---|---|---|
| `SemanticsTree` | nodes/roles/states/actions/relations, dirty diff | Scene layout |
| `SemanticsUpdater` | mapping node changes -> dirty semantics | Platform bridge |
| `FocusManager` | keyboard/screen-reader focus, scope/trap/restore | Widget behavior |
| `AnnouncementQueue` | priority/coalescing/rate limit | Message text/i18n |
| `PlatformBridge` | snapshot -> UIA/AT-SPI/UIAccessibility, actions back | Scene graph access |

Semantics update pipeline:

```text
node/style/layout/text/focus/selection change
  -> mark semantics dirty (node/subtree)
  -> coalesce at tick/frame boundary (tick: see game-loop.md)
  -> rebuild/diff SemanticsNode snapshots
  -> platform bridge reads immutable snapshot
```

- A snapshot is keyed by `(tree_revision, node_id)`; bridges never hold scene pointers.
- Removed/hidden nodes are removed from the tree before focus/announcement updates.
- Bounds are in window logical pixels; the bridge converts to screen coordinates.
- Name resolution: explicit name -> label/child text -> placeholder (inputs) -> diagnostic.
  Missing names for interactive nodes are test failures.

Actions:

```cpp
enum class SemanticsAction { Activate, Focus, SetValue, SetSelection, Increment, Decrement,
                             Scroll, Dismiss, ShowMenu, Copy, Cut, Paste };

struct SemanticsActionRequest {
    NodeId node;
    SemanticsAction action;
    ActionPayload payload;   // value/delta/scroll unit
    std::uint64_t epoch;     // reject stale actions
};
```

- Actions are validated: node exists, role supports action, state allows it, epoch current.
- Actions dispatch as UI-thread commands at a tick boundary; never from the bridge thread.
- Async actions (set value/scroll) complete through the normal event/state path; no re-entrancy.
- Secure fields redact values and reject copy/paste by policy.

Focus/announcements:

- Keyboard focus and screen-reader focus are synchronized by node id; avoid double events.
- Modal focus scope: background nodes removed/inert; restore the exact previous focus if valid.
- Announcement queue: `assertive` interrupts, `polite` waits; coalesce identical messages within
  a short window; rate-limit progress.
- Route/screen changes announce the screen title once, then focus the heading/first control.

Platform mapping notes:

- Windows UIA: providers/fragments, patterns per role; COM (Component Object Model) apartment rules; batch property
  changes; never block the UI thread on a client call.
- Linux AT-SPI: D-Bus protocol without linking libatspi; expose a bounded node subset; actions
  return immediately and complete asynchronously.
- Android/iOS/macOS: map roles/states/actions to native accessibility APIs; mobile bridges are
  deferred (Big Step F, Android first) but the semantics contract is the same.

Failure/edge cases:

- Bridge unavailable -> accessibility disabled + diagnostic; app still runs.
- Action on stale/hidden/disabled node -> rejected, no crash.
- Focus target removed -> nearest valid ancestor/root, not nowhere.
- Platform client disconnects during a snapshot -> discard, rebuild on next request.
- Text selection/caret updates are coalesced; never announce every keystroke.

Gap-closure decisions (ids G-n are gaps and Q-n are open questions, listed in [`app-completeness-cross-check-v1.md`](../architecture/app-completeness-cross-check-v1.md); resolutions in [`app-completeness-gap-resolutions-v1.md`](../architecture/app-completeness-gap-resolutions-v1.md)):

- **G-18/Q-17**: the role/action/state matrix in section 2 of this file is normative; inference is allowed only for
  text/labels, while interactive/custom/3D nodes must declare semantics. Action requests carry
  an epoch and run on the UI thread. Announcements dedupe identical messages within 250 ms and
  progress within 1 s. Unknown roles fall back to generic + diagnostic.
- **G-19/Q-18**: semantics snapshots are CI gates; Windows/Linux screen-reader smoke runs
  nightly; custom/3D elements require a non-visual fallback and a semantics snapshot. Canvas-only
  interaction is a test failure.

## 11. Deferred/open questions

- Exact per-platform role/action mapping edge cases (documented in the bridge layer).
- Multi-window/global focus.
- High-contrast token coverage in the component library.
- Switch/voice control and text navigation by word/line (after V1; no release assigned).
- Screen-reader automation on mobile (deferred until the mobile backend exists, Big Step F).

## 12. Implemented subset: deterministic semantics snapshot

`include/tessera/ui/accessibility/a11y_tree.hpp` + `src/ui/accessibility/a11y_tree.cpp` (library
target `tessera_accessibility`) implement the first semantics slice: one bounded, pre-order snapshot
of scene nodes with roles, names, states, actions, and world bounds. No platform API is called and
no action is invoked in this slice. The builder is a read-only consumer of the scene, node metadata,
computed style, focus, and scroll state.

Model and caps:

```cpp
struct A11yNode {
    NodeId id; std::uint64_t automation_id;
    Role role; State states; Action actions;
    char name[kMaxTextBytes]; char value[kMaxTextBytes]; char description[kMaxTextBytes];
    rhi::Rect bounds;                       // world bounds, window logical pixels
    A11yIndex children[kMaxChildren]; std::uint16_t child_count; bool children_truncated;
};
```

- Flat `std::vector<A11yNode>` snapshot (bounded, reused across builds): `kMaxNodes = 256`,
  `kMaxChildren = 64`, `kMaxTextBytes = 128` UTF-8 bytes per name/value/description (truncated at a
  code-point boundary). Text is copied inline, so the snapshot owns no scene strings.
- Order is pre-order (parent first, children in scene order); `children` holds indices into the
  snapshot. Bounds are `Node::world_bounds()` (AABB) and include ancestor scroll offsets.
- API: `build_a11y_tree(root, styles, out, options) -> BuildStats`,
  `find_by_automation_id`, `find_by_node_id`, `name_of`/`value_of`/`description_of`,
  `role_from_string`/`to_string`. `StyleProvider` here has the same shape as the paint provider
  (one forwarding adapter) so the layer does not depend on paint/render.
- Build registration: the library target lives in `src/ui/accessibility/CMakeLists.txt`; the root
  CMakeLists does not add the subdirectory yet, so `tests/unit/a11y_tree_tests` compiles the source
  directly. Adding `add_subdirectory(src/ui/accessibility)` is the one-line integration step
  (Lead-owned).

Role derivation (first match wins):

| Priority | Source | Rule |
|---|---|---|
| 1 | `Node::role()` | Explicit ARIA-style role name. Known names map; an unknown explicit role degrades to generic and does not fall back to the element kind. |
| 2 | `Node::element_kind()` through the schema `impliedRole` table (`tessera_schema`, read-only) | View generic, Text text, Button button, TextInput/TextArea textbox, Image/Path img, Scroll scroll view, VirtualList/VirtualGrid list, Spacer presentation; ErrorBoundary/CustomElement/unknown tags generic. |
| 3 | Fallback | generic. |

`presentation` (and `role="none"`) marks a node decorative: it is not emitted and its subtree is
skipped (`skipped_decorative`). Unknown roles never drop a node.

Name precedence:

1. `aria-label`.
2. `aria-labelledby`: whitespace-separated ids resolved by `Node::element_id()` inside the same
   scene; the first resolvable token contributes that node's `aria-label`, else its D9 paint text.
   Unresolvable tokens are skipped and the next token is tried.
3. D9 paint text (`PaintData::kind == Text`, non-empty).
4. `aria-placeholder`.
5. `Node::element_id()`.
6. Empty.

Text-field exception (documented): for `textbox` and `slider` roles the D9 text is the control's
*value*, so it is exposed as `value` and only used as a last-resort name after placeholder/id.
`value` is D9 text for text fields and `aria-valuetext`/`aria-valuenow` for slider/progress;
`description` is `aria-description`.

Hidden/ignored rules (evaluated in this order; "subtree" means the node and its descendants are not
emitted and the BuildStats counter is incremented):

| Rule | Condition | Effect |
|---|---|---|
| invisible | `Node::visible()` false | subtree, `skipped_invisible` |
| aria hidden | `aria-hidden="true"` | subtree, `skipped_aria_hidden` |
| style hidden | computed `display: none`, or `visibility: hidden`/`collapse` | subtree, `skipped_style_hidden` |
| scroll-clipped | world rect does not intersect the intersection of every ancestor scroll container's scrollport (border box inset by `scrollport_insets`) | subtree, `skipped_clipped` |
| zero size | world width or height `<= 0` | subtree, `skipped_zero_size` (no hoisting) |
| depth | deeper than 128 | `dropped_nodes` + `truncated` |
| decorative | presentation/none role | subtree, `skipped_decorative` |

Partially visible nodes are emitted with their full world bounds (the bridge clips); a scroll
container whose scrollport becomes empty stops its whole subtree with one `skipped_clipped` count.
Ignored wrappers are not hoisted in this slice (children indices stay exact).

States:

| State | Derivation |
|---|---|
| focusable | `Node::focusable()` and not disabled |
| focused | exactly one entry: `BuildOptions::focused_id` (the FocusManager's focus); a hidden focus target produces no focused flag |
| disabled | `PseudoState::Disabled` or `aria-disabled="true"` |
| selected | `aria-selected="true"` |
| checked | `PseudoState::Checked` or `aria-checked` true/`mixed` (mixed is exposed as checked; no tri-state yet) |
| expanded | `aria-expanded="true"` |

Actions (mask only; no invocation):

| Action | Derivation |
|---|---|
| focus | `Node::focusable()` (kept for disabled focusable controls) |
| press | not disabled and role is button/link/checkbox/radio/switch/tab/menuitem |
| set value | not disabled and role is textbox/slider |
| scroll | not disabled and `scrollable_x()`/`scrollable_y()` |

Disabled clears press/set-value/scroll. Actions are validated/executed by a later bridge slice
(epochs, UI-thread dispatch).

Determinism and tests: the builder reads no clock/random state and allocates nothing after the
snapshot `reserve`, so two builds of the same scene are identical. Coverage in
`tests/unit/a11y_tree_tests.cpp` (`a11y_tree`): role/name precedence matrix, hidden/zero-size/
aria-hidden/scroll-clip rules, focused uniqueness, actions and states per role, pre-order and
children indices, node/child/text caps and truncation flags, automation-id lookup, determinism,
and a combined scroll + focus + metadata scene.

Deferred: platform bridges (UIA/AT-SPI/NSAccessibility/Android), action invocation and epochs,
events/notifications/live regions, semantics diffing and dirty flags, ignored-wrapper hoisting,
tri-state checked, virtualized item realization, hidden-but-laid-out entries, text selection/
caret/IME semantics, relations (`describedby`, reverse `labelledby`), localization, and
screen-reader testing.

## 12.1 Implemented subset: in-tree AT-SPI2 D-Bus wire client

The Linux bridge speaks the AT-SPI2 D-Bus protocol directly - no libatspi, no glib, no new
third-party dependency ([ADR-012](../../decisions/ADR-012-static-linking.md)) - in
`include/tessera/ui/accessibility/` + `src/ui/accessibility/` (same `tessera_accessibility`
target). Everything is deterministic and testable without a bus daemon.

| Layer | Files | Contract |
|---|---|---|
| Address | `dbus_address.*` | Parses the `;`-separated D-Bus address list, decodes `%XX`, expands `unix:runtime=yes`, and resolves only `unix:path=`/`unix:abstract=`. Other transports (`tcp:`, `unixexec:`, `launchd:`, `autolaunch:`) return `UnsupportedTransport`, so accessibility disables instead of guessing. Input is capped at 1 KiB and endpoints at 107 bytes (`sun_path`). |
| Transport | `dbus_transport.*`, `dbus_unix_transport.cpp` | `Transport` seam (`connect`/`read_some`/`write_all`/`close`). `LoopbackTransport` is the deterministic in-process pair every test uses; `create_unix_socket_transport()` returns the POSIX `SOCK_STREAM` implementation on Linux and `nullptr` elsewhere. Reads are bounded by a `poll` timeout, never by a spin. |
| Codec | `dbus_wire.hpp`, `dbus_wire.cpp`, `dbus_reader.cpp`, `dbus_message.cpp` | Full alignment table, signature validation, `MessageWriter`/`MessageReader`, and `parse_message`. Every value is written/read at its D-Bus alignment and bounds-checked against the enclosing container; an array's declared length must match its elements. Caps: 512 bytes of text, 4 KiB of header fields, 256 KiB of body, 16 nested containers. Errors are `WireError` values. |
| Auth | `dbus_auth.*` | SASL `EXTERNAL`: NUL + `AUTH EXTERNAL <hex uid>`, then `OK`/`REJECTED`/`ERROR`/`DATA` handling, optional `NEGOTIATE_UNIX_FD` (a refusal falls back to a plain connection), then `BEGIN`. Partial lines are buffered; lines are capped at 4 KiB. |
| Mapping | `atspi_mapping.*` | Role id/name table (`AtspiRole`), the two-word `AtspiState` set, interface list, action names, text content, `AtspiValue`, component layer, and `/org/a11y/atspi/accessible/<index>` paths. |
| Dispatch | `atspi_bridge.*`, `atspi_methods.cpp` | Answers `Accessible` (`GetName`, `GetDescription`, `GetRole`, `GetRoleName`, `GetState`, `GetInterfaces`, `GetChildCount`, `GetChildren`, `GetChildAtIndex`, `GetParent`, `GetIndexInParent`, `GetApplication`, `GetAttributes`, `GetRelationSet`, `GetAccessibleId`, `Contains`), `Application` (`GetLocale`, `GetApplicationBusAddress`, `GetName`, `GetVersion`, `GetToolkitName`, `GetId`), `Action` (`GetActionCount`, `GetActions`, `GetActionName`, `GetActionDescription`, `GetKeyBinding`, `DoAction`), `Component` (`GetExtents`, `GetPosition`, `GetSize`, `GetLayer`, `GetMDIZOrder`, `GetAlpha`, `GrabFocus`, `Contains`), `Text` (`GetCharacterCount`, `GetText`, `GetTextAtOffset`, `GetCaretOffset`, `SetCaretOffset`, `GetSelectionCount`, `GetSelection`), and `Value` (`GetCurrentValue`, `SetCurrentValue`, `GetMinimumValue`, `GetMaximumValue`, `GetMinimumIncrement`). Unknown object/method/arguments return `UnknownObject`/`UnknownMethod`/`InvalidArgs`. |
| Events | `atspi_events.*` | Bounded (64) coalescing queue keyed by `(kind, node, detail)`; state/property/bounds/visible-data factories; one `org.a11y.atspi.Event.Object` signal (`siiva{sv}`) per delivered event. |
| Properties | `atspi_properties.cpp` | Serves `org.freedesktop.DBus.Properties` `Get`/`GetAll` from a fixed table: Accessible (`Name`, `Description`, `Parent`, `ChildCount`, `Interfaces`, `AccessibleId`), Application (`Name`, `ToolkitName`, `Version`, `AtspiVersion`, `Id`, `Locale`), Text (`CharacterCount`, `CaretOffset`), and Value (`CurrentValue`, `MinimumValue`, `MaximumValue`, `MinimumIncrement`). An unknown interface or property is `InvalidArgs`; `GetAll` emits the table order, which is stable. |
| Session | `atspi_session.*` | Owns the two connections and runs the handshake: session address -> UNIX connect -> SASL `EXTERNAL` -> `org.freedesktop.DBus.Hello` -> `org.a11y.Bus.GetAddress` at `/org/a11y/bus` -> connect/auth the accessibility bus -> `Hello` -> `org.a11y.atspi.Registry.RegisterEvent` for `object:state-changed`, `object:property-change`, and `object:bounds-changed`. Every step is a byte-driven state machine returning a typed `SessionError`; it never waits on a reply that has not arrived and never throws. Handshake calls are correlated by `reply_serial` against a bounded table, and an unroutable reply is `UnexpectedReply`. |

`pump(timeout_ms)` is the single I/O entry point: it reads at most four 4 KiB chunks, frames
complete messages from a bounded accumulator, answers handshake replies, dispatches AT method calls
through `AtspiBridge`, and writes auth replies, handshake calls, replies, and up to one event batch
(`kMaxEventBatch`) per iteration. A peer close after `Ready` is a clean `Closed`; before `Ready` it
is `TransportClosed`. `start()` owns exactly one worker thread and `stop()` signals and joins it
(no detached task); `pump_sync` requests one worker iteration and waits for it, which is how the
tests drive the handshake deterministically.

Threading ([ADR-021](../../decisions/ADR-021-async-runtime.md)): the UI thread only calls `publish`
(bounded snapshot copy) and `post_event`/`take_actions` (bounded ring); the session's worker thread
owns `pump` -> transport/`dispatch`/`emit_events`. Accessibility actions never run on the bridge
thread: `DoAction`, `GrabFocus`, and `SetCurrentValue` validate the request and append an
`ActionRequest` (the queue coalesces identical node/action pairs) for the UI thread to execute at a
tick boundary.

Known limits of this slice, with the deferred integration:

- The session has no reconnect/backoff: a closed or failed connection stays terminal until a host
  calls `start()` again, which reconnects from the session bus.
- `Properties` is a static read-only table: `Set` and `PropertiesChanged` are not served.
- Text offsets are UTF-8 byte offsets with one run for the whole text (no line/word boundaries).
- `Value` reports the snapshot's textual value against a fixed 0..100 range with a 1 increment,
  because the snapshot does not carry ARIA `valuemin`/`valuemax` yet.

Tests (`tests/unit/a11y/`, names start with `a11y`): `a11y_dbus_address`, `a11y_dbus_transport`,
`a11y_dbus_wire` (golden bytes for header framing, a `(so)` reply, and a `siiva{sv}` signal),
`a11y_dbus_wire_bounds` (truncation, alignment, caps, nesting, writer overflow), `a11y_dbus_auth`,
`a11y_atspi_mapping`, `a11y_atspi_events`, `a11y_atspi_bridge`, `a11y_atspi_bridge_events`,
`a11y_atspi_properties` (variant types, the `GetAll` table, error values),
`a11y_atspi_session` (byte replay of every handshake message, dispatch and event emission after
`Ready`, determinism), and `a11y_atspi_session_pump` (worker start/stop join, restart, handshake
timeout, peer close before and after `Ready`, worker-driven determinism).

