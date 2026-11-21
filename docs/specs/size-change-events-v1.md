# Size change events v1: `onSizeChange` and `SizeChangeEvent`

Status: implemented (ADR-041 item 3, issue #694). This spec is the normative contract for the
`onSizeChange` event prop and its `SizeChangeEvent` payload; the design rationale, the settle loop
and the container-query half live in [`layout-observation.md`](../design/layout-observation.md) and
[ADR-041](../../decisions/ADR-041-layout-observation.md). It builds on
[`jsx-subset-v1.md`](jsx-subset-v1.md) section 9 (handler forms),
[`jsx-intrinsics-v1/common-and-events.md`](jsx-intrinsics-v1/common-and-events.md) section 2 (the
event table), [`ir-v1.md`](ir-v1.md) section 12 (additive event names) and
[`runtime-config-v1.md`](runtime-config-v1.md) section 3 (`layoutObservationMaxRounds`).

Terms: **observed node** is the scene node an `onSizeChange` prop is written on. **Border box** is
the node's laid-out rectangle (`Node::bounds()`); **content box** is that rectangle inset by the
resolved border and padding. **Delivery** is one call of the handler. **Tick** is one fixed-step
update ([`game-loop.md`](../design/game-loop.md) section 3); a render frame between ticks is not a
tick. **Settle round** is one collect/deliver pass of the loop in
[`layout-observation.md`](../design/layout-observation.md) section 5.1.

## 1. Goals and non-goals

Goals:

- A component can react to its own laid-out size without polling, without a JS runtime and without
  a new hook (ADR-041 decision 5).
- The reaction lands in the **same tick** as the layout change that caused it, so no frame paints a
  stale layout (ADR-041 decision 3).
- Deterministic and headless-testable: the same input script under `ManualClock` produces the same
  deliveries, rounds and counters.
- A static UI stays idle: a scene with no observation and a sheet with no `@container` cost one
  boolean test per frame and present nothing.

Non-goals: per-pixel or per-render-frame callbacks; position observation (`onLayoutChange`); a
`useElementSize(ref)` hook (ADR-041 rejected it for V1); observing a node from outside the component
that owns it.

## 2. The prop

1. `onSizeChange` is accepted on every intrinsic that produces a layout box: `View`, `Text`,
   `Button`, `TextInput`, `TextArea`, `Image`, `Path`, `Scroll`, `CustomElement`, and the containers
   of `VirtualList`/`VirtualGrid`.
2. It is **rejected** on `Spacer`, `ErrorBoundary`, `Slot` and `Component` nodes: a component has no
   box of its own, so a component observes its root element instead. The rejection is a front-end
   error naming the element, not a silent no-op.
3. It is **rejected** inside a list or virtual item template: each realized row would need its own
   observation, which is a separate slice. The error names the construct.
4. All three handler forms of [`jsx-subset-v1.md`](jsx-subset-v1.md) section 9 apply: an inline
   handler, a native handler (`// Note: bind::<id>`), and a setter reference when the state type is
   `SizeChangeEvent`.
5. The event **does not bubble** and has no capture phase. It is not dispatched through
   `dispatch_event`; it is delivered by the layout observer set, so `stopPropagation` and a
   capture-phase listener have no meaning for it.

## 3. The payload

`SizeChangeEvent` is a record with four `float` members, in this order:

| Member | Meaning |
|---|---|
| `width` | Border-box width, logical pixels, unsnapped |
| `height` | Border-box height, logical pixels, unsnapped |
| `contentWidth` | Border-box width minus the resolved border and padding |
| `contentHeight` | Border-box height minus the resolved border and padding |

6. Units are **logical pixels** (dp), never physical pixels and never snapped: pixel snapping is a
   paint-time concern ([`pixel_snap.cpp`](../../src/ui/layout/pixel_snap.cpp)). A DPI change with the
   same logical size therefore delivers nothing.
7. A node with no layout box (not in the last published result, for example `display: none`) reads
   as `0x0` for all four members.
8. The whole record is the primary value: a handler that needs only the width reads `e.width`, and a
   handler that stores the size stores the record.

## 4. Delivery rules

9. **Initial delivery.** A new observation's last delivered size is "none", so the first layout that
   includes the node delivers, including `0x0`. This differs from the browser `ResizeObserver`, which
   skips a `0x0` initial observation; the difference is deliberate, because a component that starts
   hidden must still learn that it is hidden.
10. **Once per tick at most, with the latest size.** Multiple layout runs in one tick (settle rounds)
    deliver an observation at most once per round, and the value delivered is the size the last
    layout produced, not an intermediate one.
11. **Change detection is exact.** "Changed" compares the border box **and** the content box with the
    last delivered value using exact float equality. Layout is deterministic, so equal inputs give
    bit-equal results; there is no epsilon.
12. **A position-only move delivers nothing.** Size is what a callback branches on; a consumer that
    needs the position reads the layout read model or uses an overlay anchor.
13. **A size returning to its last delivered value delivers nothing.** The comparison is against the
    last *delivered* value, not the last observed one.
14. **A transform does not deliver.** Transforms do not change layout size, so a scale animation
    never fires the callback.
15. **A layout-property transition delivers once per tick while it runs.** A transition on `height`
    changes layout each tick it runs, so it delivers once per tick; that is the documented tick-rate
    ceiling, not a per-render-frame callback.
16. **Order is attach (mount) order**, so a parent is delivered before its child and the order is the
    same on every run. There is no hash order and no wall clock.

## 5. Feedback-loop bounds

17. **Depth rule.** In round `r > 1` only observations strictly deeper than the shallowest node
    delivered in round `r - 1` are delivered; shallower ones are deferred. Each round therefore
    strictly increases the depth floor, so the rounds terminate by tree depth even without a budget.
18. **Round budget.** `layoutObservationMaxRounds` (default 4, range 1..16, out-of-range values
    clamp) bounds the rounds of one tick. `1` means next-tick delivery. Each round also runs the
    update scheduler's own pass budget, so the worst case per tick is `max_rounds` layouts and
    `max_rounds * 8` update passes.
19. **Deferral, not drop.** An observation still due when the budget ends stays due, the runtime loop
    is woken, and it is collected first on the next tick. Correctness wins over idling: a stale size
    must not stay on screen.
20. **Oscillation is reported, not hidden.** A backlog that persists for 8 consecutive ticks logs
    `TESSERA_LAYOUT_OBSERVER_LOOP` once, and the settle counters
    (`layout_observation_deliveries`, `layout_observation_rounds`, `layout_observation_deferred`,
    `layout_observation_round_limit_hits`) make it visible. The loop keeps waking; it is never
    silently stopped.
21. **Equality-gated setters.** A generated `set_<state>(v)` returns without scheduling an update
    when `v` equals the current value, so a re-delivery that computes the same state is a no-op. A
    record state cell has no generated equality in this slice, so its setter always schedules.

## 6. Lifetime and threading

22. **Attach at mount, detach at unmount.** A generated component attaches its observations in
    `attach_layout_observers()` and detaches them in `detach_layout_observers()`, mirroring
    `attach_scheduler`. Developer C++ uses a move-only `LayoutObservation` that detaches in its
    destructor.
23. **Detaching is required before the node dies.** A debug build asserts that the stored node id
    still matches the node pointer's id when the set collects.
24. **Detach during delivery is safe** (the entry is tombstoned and compacted after the round); an
    observation attached during delivery is first collected in the next round.
25. **UI thread only.** Attach, detach, collect, deliver and every read assert the owner thread in
    debug builds. Another thread gets sizes through a callback that copies them into its own bounded
    queue.

## 7. Where it fires

26. The host publishes the committed layout right after `apply_layout` wrote the boxes, then runs
    the settle loop, then damage and paint. A callback therefore runs **after** layout and **before**
    paint, never mid-layout: Yoga is not re-entered from a callback, and a setter only marks a
    ticket.
27. A reaction that changes layout in the same tick is applied before damage and paint, so the frame
    never shows the stale layout.
28. A render frame between ticks never delivers: interpolation is paint-only and does not change
    layout.

## 8. Diagnostics

29. The settle counters are readable from the host (`AppHost::layout_settle_totals()`), so "a resize
    inside one condition costs a layout pass and nothing else" is asserted by a counter rather than
    inferred from a picture.
30. A handler the preview cannot run (a native binding with no preview body, or a parameter type
    other than `SizeChangeEvent`) is reported in the session's `unrun` list, never silently treated
    as "nothing happened".

## 9. Evidence

| Rule | Test |
|---|---|
| 9, 10, 11, 12, 13, 14, 16, 22, 23, 24 | `layout_observer` (`tests/unit/layout_observer_tests.cpp`) |
| 17, 18, 19, 20 | `layout_observer_settle` (`tests/unit/layout_observer_settle_tests.cpp`) |
| 21 | `codegen_schedule_runtime` (`tests/unit/codegen_schedule_runtime_tests.cpp`) |
| 1, 2, 3, 4, 5 | `schema`, `lowering_nodes`, `lowering_nodes_reject` |
| 6, 7, 8, 22 | `codegen_size_change`, `codegen_size_change_runtime` |
| 26, 27, 29 | `layout_settle_host` (`tests/unit/layout_settle_host_tests.cpp`) |
| 30 | `scene_canvas_size_change` (`tests/unit/scene_canvas_size_change_tests.cpp`) |
