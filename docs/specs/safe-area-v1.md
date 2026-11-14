# SafeArea v1

Status: implemented in part (component library stage 1,
[`component-library.md`](../design/component-library.md) section 3.2; issue #223; see section 11 for what
ships and what is deferred). Builds on the insets and fold data of
[`os-events-v1.md`](os-events-v1.md), the shell rules of [`window-chrome-v1.md`](window-chrome-v1.md)
and [`overlays-and-layers-v1.md`](overlays-and-layers-v1.md).

Terms: **inset** is the distance from a window edge that content should avoid. **Safe area** is the
window rectangle left after removing every inset. **Cutout** is a notch, punch-hole or camera
island in the display. **Fold** and **hinge** are the crease of a foldable display
([`FoldInfo`](os-events-v1.md)). **IME** is the input method editor, here the on-screen keyboard.
See also the [glossary](../reference/glossary.md).

## 1. Goals and non-goals

Goals:

- Keep content out of notches, rounded corners, system bars, the home indicator, the keyboard and a
  hinge, with one component and no per-platform code in the app.
- Edge-to-edge is the app's choice, per node, never an accident.
- Insets change smoothly (keyboard, rotation) without layout jumps.

Non-goals: drawing system bar backgrounds (a window chrome concern,
[`window-chrome-v1.md`](window-chrome-v1.md)); placing content around a cutout (that is
[`live-capsule-v1.md`](live-capsule-v1.md)); a foldable layout (`TwoPane`).

## 2. Data

`SafeArea` reads three sources from `WindowMetrics`: the `WindowInsets` (status bar, navigation
bar, display cutout, IME, system gestures), the `FoldInfo` hinge rectangle and the new `CutoutInfo`
([`os-events-v1.md`](os-events-v1.md)). It does not read OS handles. All values are logical pixels
in window coordinates and are consistent for one frame.

## 3. Component

`SafeArea` is a library component that wraps its children in padding computed from the insets.

| Prop | Type | Default | Notes |
|---|---|---|---|
| `edges` | any of `top`, `right`, `bottom`, `left` | all four | Sides that apply; a missing side draws edge to edge there |
| `sources` | any of `system-bars`, `cutout`, `ime`, `gestures`, `hinge` | `system-bars`, `cutout`, `ime`, `hinge` | Which insets count; `gestures` is off by default (it is large on gesture navigation) |
| `minimum` | length | `0` | Floor per side, so a device with zero insets still keeps a margin |
| `mode` | `padding`, `margin` | `padding` | `padding` keeps the background under the unsafe area; `margin` leaves it empty |

Insets are logical start/end aware: `left`/`right` swap for right-to-left layouts only in the
`edges` shorthand `start`/`end`, which the component also accepts.

The resolved insets are published to descendants as `safeAreaInsets` in the layout environment, so
a nested component (a sticky header inside a scroll view) can read them without a second
`SafeArea`.

## 4. Behavior

- **Keyboard (IME).** The bottom inset grows with the keyboard. The change is animated with the
  platform's insets animation when it provides one (Android `WindowInsetsAnimation`, iOS keyboard
  notification curve); otherwise with a short ease-out. Reduced motion applies the final value at
  once. A focused text field scrolls into view.
- **Rotation and fold changes.** Insets and the hinge rectangle update on `SafeAreaChanged` and
  `FoldChanged`; one layout pass applies them, never a flash of stale padding.
- **Hinge.** With `hinge` in `sources` and `FoldOcclusion::Full`, the rectangle across the hinge is
  treated as an unsafe band: content that spans it is split by `TwoPane`, and a single-pane
  `SafeArea` pads away from it.
- **Defaults of other components.** `AppBar`, `BottomNavigation`, `Drawer`, `Sheet` and `Toast`
  apply the relevant insets themselves; content opts out only by setting `edges` on the wrapping
  `SafeArea`.

## 5. Cutout fallback

When `CutoutInfo.count == 0` (iOS, desktop, web in V1) the top inset of the system bars is the only
information. A consumer that needs a cutout rectangle, such as `LiveCapsule`, then uses a rectangle
as wide as zero at the top center, below the top inset. `SafeArea` itself does not need the
rectangle: the cutout inset already keeps content clear of it.

## 6. Power and threading

Insets are read once per frame from the immutable `WindowMetrics`. A static layout with unchanged
insets causes no layout, paint or present. The component allocates nothing per frame.

## 7. Accessibility

`SafeArea` has no role and is ignored by the semantics tree (a pure layout wrapper, the "ignored
wrapper hoisting" rule of [`accessibility-v1.md`](accessibility-v1.md)). Content moved out from
under a system bar stays reachable in focus order.

## 8. Styling parts

None. It is a layout wrapper; the background belongs to the child or to `mode="padding"` plus the
parent's style.

## 9. Testing

- Unit: inset arithmetic per `edges`, `sources`, `minimum`, `mode`, and logical start/end under
  right-to-left, all with a fake `WindowMetrics`.
- Headless: scripted `SafeAreaChanged`, `ImeChanged` and `FoldChanged` events and layout
  snapshots, including a notched phone, a gesture-navigation phone, a tablet with a bottom
  keyboard and a book-mode foldable.
- Golden: one screen with each of those metrics; default and dark themes.
- Animation: keyboard inset animation under the manual clock; reduced motion jumps.

## 10. Decisions

Decided by the owner on 2026-11-13:

1. **Per-edge animation:** each edge animates independently, so a rotation with the keyboard open
   does not jump.
2. **Custom desktop title-bar insets** stay in [`window-chrome-v1.md`](window-chrome-v1.md);
   `SafeArea` does not read them.
3. **Rounded screen corners:** a rectangle only in V1; a corner radius is exposed when a
   component needs it.

## 11. Implementation (issue #223)

How the spec above is realized, and what is not built yet.

- **Data.** `WindowMetrics` gained `ime_insets`, `gesture_insets` and `fold` (`FoldInfo`), and the
  event types `ImeChanged` and `FoldChanged` (consecutive ones coalesce, like `SafeAreaChanged`).
  `insets` stays the system bars. The cutout inset is derived from the cutout rectangles: a rectangle
  that abuts a window edge (within 1 px) is an inset on that edge, plus the waterfall insets.
- **Arithmetic.** `hal::resolve_safe_area` (`tessera/hal/safe_area.hpp`) takes, per side, the
  largest inset of the enabled sources (they overlap: the IME inset already includes the bars under
  it), raises it to `minimum`, and zeroes sides `edges` does not select. `start`/`end` swap under
  right-to-left. A full-occlusion hinge makes a single pane keep the larger side of the hinge; on a
  tie it keeps the first (left or top).
- **No layout environment.** Instead of publishing `safeAreaInsets` for descendants, the host writes
  the result as the SafeArea node's `padding-*` (or `margin-*`) before layout, so a nested component
  needs no second `SafeArea` and no hidden context. The host reads the node's class tokens
  (`tsafe`, `safe-edges-top+bottom`, `safe-sources-ime`, `safe-min-8`, `safe-mode-margin`) because
  the JSX subset has no custom-property `style` keys and `Node` has no room for a member. The
  tokens are the component's private contract; apps use the props.
- **Props as strings.** `edges` and `sources` are `+`-joined strings (`"top+bottom"`) and `minimum`
  is whole pixels, until the subset has a set or length prop type. The instance host node is styled
  by the component name (`SafeArea` in `safe-area.css`) so the wrapper fills its parent.
- **Animation.** `AppHost::set_window_metrics` applies the first metrics at once. Later changes
  ease the system bars, the keyboard and the gesture zones per edge (60 ms time constant, in the
  host, so it is deterministic under the manual clock); reduced motion jumps. Cutout and hinge
  rectangles are geometry and apply at once. Unchanged insets cause no layout; a settled animation
  stops requesting frames.
- **Not built yet.** `AppBar`, `BottomNavigation`, `Drawer`, `Sheet` and `Toast` defaults (those
  components do not exist), `TwoPane`, scrolling a focused text field into view, the platform's own
  insets animation curve (Android `WindowInsetsAnimation`, the iOS keyboard curve; needs the
  platform backends) and an Android/iOS fill of the new fields.

Evidence: ctest `safe_area`, `app_safe_area`, `library_safe_area_lowering`,
`library_safe_area_runtime` (notched phone, gesture phone, tablet keyboard, book-mode foldable) and
`gallery_golden_safe_area_*` (four devices in the light and dark themes).
