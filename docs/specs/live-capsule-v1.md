# LiveCapsule v1

Status: proposed (component library stage 2, [`component-library.md`](../design/component-library.md)
section 3.3; ships after V1). Needs `CutoutInfo` from [`os-events-v1.md`](os-events-v1.md). Builds on
[`safe-area-v1.md`](safe-area-v1.md), [`overlays-and-layers-v1.md`](overlays-and-layers-v1.md),
[`animation-v1.md`](animation-v1.md) and [`accessibility-v1.md`](accessibility-v1.md).

Terms: **capsule** is the pill-shaped floating surface. **Activity** is one ongoing thing the capsule
shows (a timer, playback, a transfer, a call, a recording). **Cutout** is a notch, punch-hole or
camera island ([`CutoutInfo`](os-events-v1.md)). **Spring runtime** is the physics animation
runtime of [`animation-v1.md`](animation-v1.md). **Live region** is an accessibility region whose
changes are announced. See also the [glossary](../reference/glossary.md).

The capsule is an in-app component. It is not the operating system's live-activity feature (iOS
Live Activities, Android ongoing notifications); mirroring to those is a stage-6 platform
integration, not part of this spec.

## 1. Goals and non-goals

Goals:

- Show ongoing activity at the top of the window without leaving the current screen.
- Read as an extension of the hardware cutout where one exists, and fall back cleanly where not.
- Zero cost when idle; no stolen focus; fully keyboard and screen-reader reachable.

Non-goals: system-level live activities and lock-screen surfaces; notifications history; a general
toast queue (`Toast` is separate); drawing outside the app window.

## 2. Placement

The capsule lives in the `toast` layer of the `OverlayHost`
([`overlays-and-layers-v1.md`](overlays-and-layers-v1.md)) and is positioned from window metrics:

| Condition | Position |
|---|---|
| `CutoutInfo.count >= 1` and a cutout touches the top edge | Centered on that cutout; the compact state wraps it with leading and trailing slots |
| Several top cutouts | The widest one |
| No cutout rectangles (iOS, desktop, web in V1) | Top center, `8` logical pixels below the top safe-area inset |
| Landscape, cutout on a side edge | Falls back to the no-cutout rule; the capsule never docks to a side edge |

The position is recomputed on `SafeAreaChanged`, rotation and fold changes. The capsule never moves
under the hinge: a hinge rectangle that overlaps it shifts the capsule to the larger pane.

## 3. States

| State | Content | Size |
|---|---|---|
| `minimal` | An icon or dot (one slot) | Smallest, at most the cutout width plus one dot |
| `compact` | Leading and trailing slots around the cutout (or a single row when there is none) | Cutout width plus the slots, capped at `60%` of the window width |
| `expanded` | A card: title, content slot, up to two actions | Capped at `90%` of the window width and `40%` of its height |

Transitions: a tap on `minimal` or `compact` expands; a tap outside, `Escape` or an upward swipe
collapses to the previous state. Width, height and corner radius morph together with the spring
runtime. With reduced motion the states cross-fade with no size animation. The capsule's size
changes never relayout the content below it (it floats; the page does not move).

## 4. Activities

An app-level `LiveCapsuleHost` owns a queue of activities; the component draws the front of it.

```cpp
struct Activity {
    ActivityId id;              // stable; reusing an id updates the activity in place
    Priority priority;          // low, normal, high; high preempts
    Slots slots;                // minimal icon, compact leading/trailing, expanded content
    std::optional<Duration> timeout;
};
```

- At most one `compact` or `expanded` activity plus one `minimal` activity are visible; the others
  wait ordered by priority, then arrival.
- A `high` activity takes the front and the previous one goes back to the queue, not to the void.
- `start(activity)`, `update(id, slots)` and `end(id)` are the only operations. An activity ends
  explicitly or when its `timeout` expires; there is no implicit end on navigation.
- Slots are ordinary component content, so a progress ring, a timer or a waveform can be drawn;
  frequently changing content (a ticking timer) updates its own node without relayouting the
  capsule.

## 5. Interaction and focus

- The capsule is focusable and in the tab order after the main content; it never takes focus when
  an activity starts or ends.
- `Enter` or `Space` expands, `Escape` collapses, and actions in the expanded card are ordinary
  buttons in tab order. Focus returns to where it was when the capsule collapses.
- Pointer events on the capsule do not reach the content below; pointer events outside it do.
- The capsule is excluded from dismiss-on-outside-click of other layers (it is not modal).

## 6. Accessibility

- Role `status` with a name from the front activity, plus a **polite** live region: starting and
  ending an activity is announced once, coalesced with the same message within the announcement
  window ([`accessibility-v1.md`](accessibility-v1.md)); the minimal and compact tick of a timer is
  not announced.
- Expanded content is reachable by keyboard and screen reader; the actions have names.
- High contrast keeps a visible 1 px border; the capsule never relies on color alone to show state.

## 7. Power and threading

- An idle capsule (an activity whose content does not change) draws nothing new and wakes nothing.
- Content updates follow render-on-demand: a timer wakes the pacer at its own tick rate (once per
  second for a seconds timer), not at the display rate.
- Spring animation wakes the pacer only until the spring settles.
- `start`, `update` and `end` run on the UI thread at a frame boundary
  ([`overlays-and-layers-v1.md`](overlays-and-layers-v1.md) section 5 rule); from other threads they
  post through the completion queue.

## 8. Styling parts

`capsule`, `leading`, `trailing`, `minimal-icon`, `card`, `title`, `content`, `actions`. Part names
are public API
([ADR-047](../../decisions/ADR-047-component-library-layers-and-design-language.md)). The pill
shape is an allowed exception to the small-radius rule of that ADR because the shape carries the
meaning.

## 9. Testing

- Unit: placement from fake `CutoutInfo` (one cutout, several, none, landscape, hinge overlap),
  queue ordering and preemption, `timeout`, in-place update by id, state transitions.
- Headless: semantics snapshot (role, name, live-region announcements), scripted tap, `Escape`,
  tab order, focus return; an activity starting never changes the focused node.
- Golden: each state with a notch, a centered punch-hole and no cutout; default, dark and
  high-contrast themes; mid-transition frames under the manual clock.
- Power: an idle activity records zero presents over N ticks.

## 10. Open questions

1. **Cutout on non-Android platforms:** whether iOS can supply the Dynamic Island rectangle
   without private API (it reports insets only), so iOS stays on the fallback until it can.
2. **Several simultaneous compact activities** (two compacts split left and right of the cutout):
   the first release shows one.
3. **Mirroring to system live activities** (stage 6): which activity kinds map to which platform
   feature.
4. **Gesture conflicts** with a system swipe-down shade at the top edge.
