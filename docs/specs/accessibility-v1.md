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

Implemented on the collection side: `virtualization::CollectionSemantics`
([`collection_semantics.hpp`](../../include/tessera/ui/virtual/collection_semantics.hpp), see
[`virtualization-v1.md`](virtualization-v1.md) section 7) supplies what this section requires. It
reports the collection's item count as the set size and each realized item's 1-based `pos_in_set`
from the key index; it exposes the realized focused item as the active descendant, keeps it across
realize/recycle, and pairs each realized `ItemKey` with the item node's compiler-assigned `UiId`
(the `(UiId, ItemKey)` identity of [`automation-ids-v1.md`](automation-ids-v1.md) section 1, never
re-hashed at runtime). Focus and selection changes produce bounded, coalesced announcements: the
host observes once after input handling and drains one structured announcement per observed change
(`pos_in_set`, `set_size`, realized state), so an unrealized-item focus announces exactly once with
its position, a superseded change is dropped, and nothing is announced per frame. The semantics tree
builder and the platform bridges do not consume it yet; that integration is the remaining work.

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


## 12.2 Implemented subset: Windows UIA provider on an MTA bridge thread

The Windows bridge serves UI Automation (UIA) providers from one dedicated, COM-initialized bridge
thread (docs/design/accessibility.md section 3, ADR-021). It reuses the section 12 semantics snapshot
verbatim: no second tree, no scene pointers across the boundary. Everything lives in
`include/tessera/ui/accessibility/uia_*.hpp` and `src/ui/accessibility/uia_*.cpp` (same
`tessera_accessibility` target) and is deterministic to test without a UIA client.

| Layer | Files | Contract |
|---|---|---|
| Errors | `uia_error.*` | `UiaError` values (`UnsupportedPlatform`, `ApartmentUnavailable`, `NotStarted`, `NoSnapshot`, `UnknownNode`, `PatternNotSupported`, `ElementNotEnabled`, `ThreadViolation`, `QueueFull`, `Timeout`, ...); thrown by nobody, returned everywhere. |
| Mapping | `uia_mapping.*` | UIA property/pattern/control-type ids repeated as plain integers (the Windows probe static_asserts them against `UIAutomationClient.h`); `control_type_for(Role)` and `patterns_for(A11yNode)`. Switch maps to CheckBox (UIA has no switch), alert to Text, containers to Pane. Patterns: Invoke from `Action::Press`, Value from `Action::SetValue`, SelectionItem for selectable roles or `State::Selected`, Selection for List/Menu. |
| Handoff | `uia_handoff.*` | Single-producer/single-consumer one-slot handoff. `publish` replaces an unconsumed snapshot and increments `dropped` (drop-oldest, newest tree wins); `take` copies into the consumer's reused buffer. The slot keeps its reserved capacity, so the UI-thread path allocates nothing after the first snapshot. |
| Provider | `uia_provider.*` | Bridge-thread projection of one node (`UiaElementInfo`: control type, patterns, bounded name/value/description, automation id, bounds, enabled/focused/selected), a precomputed parent table, `element`/`child_count`/`child_at`/`focused`/`hit_test`, and a bounded (32) coalescing action ring with a `SetValue` text payload. Every query asserts bridge-thread ownership. |
| Bridge | `uia_bridge.*`, `uia_bridge_jobs.*` | Owns the one bridge thread, the apartment, the handoff, and a bounded (8) client-job ring. Phases `Idle -> Starting -> Running -> Failed/Stopped`; `start` never blocks on COM initialization, `stop` signals and joins with a liveness bound, restart re-initializes the same apartment. Jobs run inline when the caller is already the bridge thread; `post` enqueues work without waiting. The newest published tree is adopted immediately before each job runs, and `set_hooks` installs the bridge-thread lifecycle hooks the host uses to own the root provider. |
| Host | `uia_host.*`, `uia_platform*.cpp` | `UiaHost` is the window/host owner: it creates the root provider on the bridge thread (the start hook) and releases it before the apartment shuts down (the stop hook), publishes only dirty snapshots, derives and queues events, drains actions at a tick boundary, and answers `WM_GETOBJECT` from one atomic pointer. `UiaHost::set_host_handle` forwards the window's opaque `hal::AccessibilityHostHandle` to the platform (window thread, atomic store only); `UiaHost::host_element_status` reports the structured wiring state. `UiaPlatform` is the platform seam: the Windows implementation owns the COM root, the cached `UiaHostProviderFromHwnd` host element, and the bounded per-node provider cache; every other platform returns nullptr, so `start` reports `UnsupportedPlatform` and the app runs with accessibility off. |
| Events | `uia_events.*` | Bounded (64) coalescing queue plus the pre-order tree diff that derives one root `ChildrenInvalidated`, per-node child-list changes (`Reordered` when only the order moved), name/value/help-text/enabled/selected/bounds property changes, and one focus event. Windows raises a batch with `UiaRaiseStructureChangedEvent`, `UiaRaiseAutomationPropertyChangedEvent` and `UiaRaiseAutomationEvent` (focus) from the cached providers on the bridge thread. |
| Window seam | `include/tessera/hal/window.hpp`, `src/hal/window/window_accessibility_win.hpp`, `src/platform/windows/window_accessibility.cpp` | `hal::Window::set_accessibility_root` installs a non-blocking answer callback. The Win32 window procedure answers `WM_GETOBJECT` with `UiaReturnRawElementProvider(hwnd, wParam, lParam, root)` and falls back to `DefWindowProc` when no root is published (starting, failed, stopped, or a non-UIA object id), so UIA keeps the default MSAA bridge instead of seeing an empty provider. `hal::Window::accessibility_host_handle()` returns the same window as an opaque `hal::AccessibilityHostHandle`; only the platform/UIA layer interprets the token, so no HWND crosses into a UI-layer type. |
| Apartment | `uia_apartment*.cpp` | Windows: `CoInitializeEx(nullptr, COINIT_MULTITHREADED)` + `CoUninitialize` + a bounded `PeekMessage`/`DispatchMessage` pump; `RPC_E_CHANGED_MODE` is `ApartmentUnavailable`. Every other platform: an apartment whose `init` returns `UnsupportedPlatform`, so accessibility disables instead of failing the app. |
| COM adapter | `uia_com*.cpp`, `uia_com_provider.hpp` | `IRawElementProviderSimple`, `IRawElementProviderFragment` (+Root for index 0), `IInvokeProvider`, `IValueProvider`, `ISelectionItemProvider`. Properties: Name, ControlType, IsEnabled, BoundingRectangle (`VT_ARRAY | VT_R8`), HasKeyboardFocus, IsOffscreen, AutomationId, HelpText, FrameworkId, ValueValue, ValueIsReadOnly, SelectionItemIsSelected; unsupported ids are `VT_EMPTY` + `S_OK`. The root's `get_HostRawElementProvider` returns the window's `UiaHostProviderFromHwnd` provider (built once on the bridge thread and cached), or `E_FAIL` when no handle/source/bridge is connected; a child fragment keeps `S_OK` + null. Runtime id is `(UiaAppendRuntimeId, index + 1)`. `get_ProviderOptions` reports `ServerSideProvider | UseComThreading`. |

Threading is the whole point of the slice:

- The UI thread only calls `publish` (bounded handoff) and `take_actions` (bounded ring drain). It
  never calls UIA, never touches COM, and never waits on the bridge thread.
- The bridge thread initializes the MTA apartment, adopts the newest snapshot immediately before
  each job runs, runs at most `max_jobs_per_iteration` (4) jobs, then pumps messages once. An idle
  bridge does no snapshot work at all: publication does not wake it, so a static window produces no
  accessibility CPU beyond the bounded idle wait (16 ms default).
- Every COM method marshals onto the bridge thread through `UiaBridge::run_on_bridge`, which runs
  inline when the client call already arrives on that thread. A client call from any other thread
  waits at most 5 s (liveness bound, not a UI wait) and otherwise returns
  `UIA_E_ELEMENTNOTAVAILABLE`.
- Actions never run on the bridge thread: `Invoke`, `SetFocus`, `Select`, and `SetValue` validate
  against the snapshot and append to the action ring; the host drains it at a tick boundary. After
  `stop()` every snapshot-dependent call returns `UIA_E_ELEMENTNOTAVAILABLE` instead of reading
  stale state; snapshot-free properties (IsOffscreen, FrameworkId) still answer.
- No exception crosses the thread or COM boundary: allocation failures are `OutOfMemory`, and a
  host callback that throws is reported as `Internal` (spec section 10, ADR-025).

Host wiring (`WM_GETOBJECT`):

- `UiaHost::start()` only spawns the bridge. The bridge thread initializes the apartment and then
  runs the start hook, which creates the root provider (`A11yIndex 0`) and publishes its raw pointer
  in an atomic. Until that pointer exists (idle, starting, failed, stopped, or a build without a UIA
  platform seam) the window falls back to `DefWindowProc`; the UI thread must never wait for COM or
  call UIA, and the answer path is one atomic load.
- `hal::Window::set_accessibility_root` is the seam the window procedure calls. The Win32 window
  answers `WM_GETOBJECT` with `UiaReturnRawElementProvider(hwnd, wParam, lParam, root)` only for the
  UIA root object id (`UiaRootObjectId`, -25); `OBJID_CLIENT` and child ids keep the platform
  default. `stop()` clears the pointer before the stop hook releases the provider, so a shutdown
  race sees "not ready" rather than a dangling object.
- The native handle crosses the seam as `hal::AccessibilityHostHandle`: `UiaHost::set_host_handle`
  stores the opaque token on the platform (window thread, atomic store only, idempotent for the same
  token). The root's `get_HostRawElementProvider` marshals to the bridge thread, where the platform
  calls `UiaHostProviderFromHwnd` once, caches the provider, and returns it `AddRef`'d. A missing
  handle, a stopped bridge, or a failed build answers `E_FAIL` with a null out-parameter; a failure is
  latched per installed handle, so a client retry does not repeat the COM call. The structured
  diagnostic is `UiaHost::host_element_status()` (`Unavailable`/`Pending`/`Ready`/`Failed` plus a
  failure count), never one log line per client call.
- `make_window_root_provider(UiaHost&)` builds the `hal::AccessibilityRoot` answer and
  `hal::Window::set_accessibility_root` installs it; reinstalling the same host is idempotent. The
  windowed POC installs it (plus a static demo pane) on both windowed paths, so a real window answers
  UIA requests; offscreen/null backends expose no host handle and are unaffected.
- `UiaHost::mark_dirty()` is called from the semantics/layout/focus change paths and `tick()` is the
  UI-thread boundary: it builds a snapshot only when dirty, publishes it, then drains at most
  `max_actions_per_tick` (8) validated actions in arrival order. A static window (not dirty, no
  queued actions) does no build, no publish, no bridge wake, and no UIA call. A throwing snapshot
  source keeps the previous tree published (and the host dirty) and is reported as a value; a
  throwing action ends that tick's batch and is counted as failed, never retried.
- One tick posts at most one event-flush job (`UiaBridge::post`, never a wait). A full job ring
  defers the flush to a later tick and the coalesced events stay queued; the queue is bounded (64)
  and drops the oldest on overflow with a counter.

Caps: 256 snapshot nodes, 32 queued actions, 8 queued client jobs, 64 queued events, 8 actions per
tick, 4 jobs per iteration, 5 s per-call liveness bound, 2 s stop liveness bound, 16 ms idle wait.
The COM object's reference count is atomic; the host owns every provider it creates (the Windows
platform caches one per snapshot index and one `UiaHostProviderFromHwnd` host element, and releases
both with the root before destroying the bridge).

Tests (`tests/unit/a11y/`, names start with `a11y`): `a11y_uia_mapping` (role/control-type/pattern
tables, run on every platform), `a11y_uia_handoff` (drop-oldest counter, newest-tree-wins, buffer
reuse), `a11y_uia_bridge` (publication only on client calls, overflow, thread ownership, action
coalescing and the QueueFull bound), `a11y_uia_bridge_lifecycle` (start/stop/restart ordering,
init failure as a value, bounded client wait while the pump is blocked, pending snapshot across a
restart), `a11y_uia_provider` (projection, parent/child, focus/hit-test, action validation),
`a11y_uia_events` (coalescing/dedup, drop-oldest, the deterministic tree diff),
`a11y_uia_host_getobject` (root handshake on the bridge thread, not-ready fallback, shutdown order,
failed init, restart), `a11y_uia_host_tick` (a static tick publishes nothing, dirty-only publish,
tick-boundary action drain and its budget), `a11y_uia_host_events` (one raise batch per flush on the
bridge thread, unchanged trees raise nothing), `a11y_uia_compile_check` (Windows: SDK headers, id
static_asserts, `uiautomationcore`/`ole32` link), `a11y_uia_window_getobject` (Windows: the real
window procedure routes `WM_GETOBJECT` through the handshake and falls back after uninstall), and
`a11y_uia_com` / `a11y_uia_com_nav` (Windows: the real provider vtable, properties, patterns,
navigation, shutdown behavior; no UIA client), `a11y_uia_com_host_element` (Windows: the root's
`get_HostRawElementProvider` contract with a fake `UiaHostProviderFromHwnd` source: AddRef'd
provider, cached across queries, `E_FAIL` for no source/unavailable/failed/stopped, `S_OK` + null
for a child), and `a11y_uia_window_host_element` (Windows: the real window + real platform seam
answer with the window's HWND, cache the provider, and fail cleanly without a handle; the client
query runs on a helper thread while the window thread pumps, so no screen reader is needed).

Known limits of this slice:

- A `UiaHostProviderFromHwnd` build failure is latched until the handle changes: a recreated window
  must call `UiaHost::set_host_handle` with the new handle (the POC does this at construction). The
  host element is built on the first query, not eagerly at `start`, so a client that never asks for
  it pays nothing.
- `UiaRaiseAutomationPropertyChangedEvent` is raised with empty `VARIANT` old/new values; clients
  re-query the property. Focus events are only raised when the host marks the snapshot dirty for the
  focus change (the event is derived from the snapshot diff, not from the input path directly).
- The tree diff keys nodes by `NodeId` and is linear per node over the bounded snapshot (worst case
  quadratic at 256 nodes) on a dirty publish only.
- `ISelectionProvider` (list-wide selection) and `ITextProvider` ranges are not advertised;
  `AddToSelection`/`RemoveFromSelection` return `UIA_E_NOTSUPPORTED` because multi-select is not in
  the semantics model.
- `ValuePattern.SetValue` text is truncated to `kMaxTextBytes` at a byte boundary, so a long
  non-ASCII value can end in a partial UTF-8 sequence; the UI thread validates before applying it.
- `IsOffscreen` is always false: the snapshot carries world bounds, not a viewport intersection.

### Manual screen-reader smoke checklist (Narrator/NVDA)

Run on a real desktop session (not a headless CI agent), from the repository root:

```powershell
.\build\vs2026\tools\poc\batched2d\Release\batched2d_poc.exe --backend vulkan --windowed --on-demand --run-seconds 60
```

- [ ] Narrator (Win+Ctrl+Enter): focus the POC window; Narrator announces "Tessera windowed POC" and
      a pane. The announcement appears once, not once per frame.
- [ ] Narrator scan mode (Caps+Space, then arrow keys): the accessibility tree contains exactly the
      demo pane and its bounds match the window's client area.
- [ ] NVDA: focus the window with NVDA running; the speech viewer shows the same pane name, and
      NVDA's element list (`NVDA+F7`) lists one pane with no error dialog.
- [ ] Resize the window while a screen reader is attached: the pane's bounding rectangle follows the
      client area and the window never hangs (input and painting stay responsive).
- [ ] Close the window with Narrator/NVDA attached: the process exits normally, with no orphaned
      provider, no crash dialog, and no multi-second stall on close.
- [ ] Static window: with no frames due and a screen reader attached, Task Manager shows ~0% CPU for
      the process (the static tick does no UIA work).
- [ ] Accessibility off (a non-Windows build or a failed UIA start): the app runs, the window keeps
      its default provider, and no error is reported to the user.
