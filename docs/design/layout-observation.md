# Layout observation: size changes, layout reads, and container queries

Status: design only (accepted in [ADR-041](../../decisions/ADR-041-layout-observation.md)).
Nothing here is implemented. Written 2026-11-11 in answer to "is there a component listener for
size changes, like the DOM `ResizeObserver`?". Builds on
[ADR-020](../../decisions/ADR-020-window-resize-and-lifecycle.md) (window resize and lifecycle),
the update scheduler of issue #5 (`include/tessera/ui/scene/update_scheduler.hpp`), the frame
order of [`app-host.md`](app-host.md) and the tick of [`game-loop.md`](game-loop.md) section 3.

## 1. Problem

Today a component cannot learn its own laid-out size. That was deliberate: the open-questions
register entry `design/window-resize.md#4` keeps resize internal and defers any public
`onResize`/`ResizeObserver` "until container queries ship", because an OS-cadence callback would
leak platform behavior into components (ADR-020 section 1 and its rejected alternative "let
styles/components observe OS resize events"). `@container` is parsed but not evaluated
([`stylesheet-v1.md`](../specs/stylesheet-v1.md) section "Later"; [`responsive.md`](responsive.md)
section 10).

Real apps still need size-dependent behavior. The concrete cases this design serves:

| # | Use case | Needs |
|---|---|---|
| U1 | A multiline `TextArea` that grows with its text | intrinsic height from text layout |
| U2 | A panel that switches layout or behavior on its own width, not the window's | a size predicate per element |
| U3 | Reading a node's box after layout to place a popover or scroll an item into view | a post-layout read |
| U4 | A virtual list reporting measured item heights | already exists (section 2.6) |
| U5 | Telemetry reading sizes | a post-layout read or notification |

Out of scope: per-pixel or per-render-frame callbacks (for example "call me on every frame of a
height animation"). Delivery is at most once per tick and never at the render (interpolation) rate.

Two facts shape the design. First, ADR-020's objection was to **OS-cadence** events. A
notification that fires after Tessera's own layout, once per tick, from the deterministic tick
order, is a different thing: it never sees an OS message, a window handle or the resize mode, and
it behaves identically under `ManualClock`. [`window-resize.md`](window-resize.md) section 3
already anticipated exactly that ("delivered after layout on the UI thread, with a generation
counter; it is not the raw OS resize event"). Second, container queries do not remove the need:
they change styles, not structure or behavior (U2 "switch behavior"), and they need the same
"layout result feeds back into style" mechanism as an observer (section 5.4).

## 2. Prior art

### 2.1 Browser `ResizeObserver`

Callbacks run after layout and before paint, in the same frame. A callback may change the DOM,
so the browser re-runs style and layout and gathers observations again, but only for targets
**deeper** in the tree than the shallowest target delivered in the previous round. When undelivered
observations remain that fail the depth rule, it fires the "ResizeObserver loop completed with
undelivered notifications" error and delivers them next frame. Observable boxes: `content-box`
(default), `border-box`, `device-pixel-content-box`. Initial observation is delivered unless the
element is 0x0 (a known gotcha). Lessons: post-layout same-frame delivery avoids a visible flash;
the depth rule guarantees termination per frame but an app bug still oscillates across frames; the
loop error is the only signal, and it is famously noisy.

### 2.2 DOM reads (`getBoundingClientRect`)

Reads force synchronous layout. Interleaving writes and reads causes layout thrashing. Lesson: a
read must never trigger layout; it returns the last committed result.

### 2.3 Flutter

`LayoutBuilder` rebuilds a subtree during layout with the parent's constraints, so the child
depends on what it is given, never on its own measured size (no loop by construction).
`SizeChangedLayoutNotifier` dispatches a notification after layout; reacting to it schedules work
for the next frame. Lesson: constraint-driven building (like container queries) is loop-free;
measured-size reactions are a one-frame-later concern.

### 2.4 Android

`View.OnLayoutChangeListener` fires after layout with old and new bounds; a `requestLayout()` from
it runs on the next pass (with a log warning when requested during layout). Jetpack
`WindowSizeClass` gives coarse window classes (compact/medium/expanded), the same buckets as
Tessera's `useBreakpoint()`. Lesson: coarse classes cover most responsive needs; fine-grained
listeners are for the rest.

### 2.5 SwiftUI

`GeometryReader` exposes the proposed size to its content (and greedily takes all space, a common
pitfall). `onGeometryChange(for:of:action:)` (iOS 18) transforms the geometry into an `Equatable`
value and calls the action only when that value changes. Lesson: observe a **derived value** with
equality, not raw geometry; it removes most redundant callbacks and most loops.

### 2.6 CSS container queries and Tessera's own virtualization

`container-type: inline-size` applies size containment on the inline axis so the container's
width cannot depend on its content; `@container (min-width: 600px)` then restyles descendants.
Containment is what makes it loop-free. Tessera already has one layout feedback path:
virtualization measures realized items after layout and feeds the sizes into
`ItemGeometry::refine_measure`, re-anchoring the scroll offset
([`virtualization-v1.md`](../specs/virtualization-v1.md) sections 4 and 6). It is bounded,
allocation-free and engine-owned; it never runs app code. This design keeps it as is (U4) and
borrows its rules: post-layout, bounded, deterministic order.

## 3. Options

| Option | Summary | For | Against |
|---|---|---|---|
| A. Nothing new | Window metrics, media queries, `useBreakpoint()` only | No new surface | U1-U3 unsolvable in TSX; developers poll in effects |
| B. Container queries only | Evaluate `@container` | Declarative, designer-friendly, loop-free with containment | Styles only: cannot switch behavior or structure; no read for U3/U5; large stylesheet slice |
| C. Next-tick observer | Notify after layout; reactions apply next tick | Simplest loop story | One visible frame with stale layout (a flash) whenever a reaction changes layout |
| D. Same-tick observer with bounded settle rounds | Notify after layout; if a callback changes state, re-run update, style and layout in the same tick, bounded | No flash; termination is bounded; C is the 1-round special case | Extra layouts on ticks where a callback changes layout; needs a host loop |
| E. Imperative reads only | `layout_info(node)` query, no notification | No loop at all | Polling; no reaction to content-driven growth |
| F. `useElementSize(ref)` hook | A hook returning a size | Familiar to React users | Needs hook parameters, a new lowering kind and ref members (decided "none in V1", react-compat Q6); same loop as D underneath |

Recommendation: **D + E + intrinsic sizing (U1) now, container queries (B) next on the same
settle mechanism**. Reject F for V1 (section 6.4).

## 4. Recommendation in one page

1. **Query side (E): a layout read model.** After each layout the host publishes the committed
   `LayoutResult` into a `LayoutReadModel`. `layout_info(node)` returns a `LayoutInfo` value
   snapshot with a generation number. Reading never runs layout (no thrashing). Serves U3, U5.
2. **Notification side (D): size observations.** A node can carry one or more observations. After
   layout, the host compares each observed node's size with the last delivered size and calls the
   observation once per tick when it changed. Fires initially. Serves U2, U5.
3. **TSX surface: an `onSizeChange` event prop** with a `SizeChangeEvent` payload, using the three
   existing handler forms. No new hook, no new IR field. Serves U2 in TSX.
4. **Intrinsic sizing for U1: `field-sizing: content`** on `TextArea` (the CSS standard property),
   clamped by `min-height`/`max-height`. One layout pass, no callback.
5. **Container queries (B) second**, as the primary answer for pure styling. They reuse the settle
   rounds of item 2, because a container's size feeds style which feeds layout.
6. **Equality-gated setters**: a generated `set_<state>` returns early when the new value equals
   the old one. It turns most re-deliveries into no-ops and is the SwiftUI lesson of section 2.5.

Container queries are the **primary answer for styling** (declarative, previewable, no app code
in the loop), and the observer is the **escape hatch for behavior and reads**. They ship in the
reverse order because the observer is smaller, its settle loop is the infrastructure container
queries need, and U2-behavior and U3 cannot wait for a stylesheet slice.

## 5. Runtime design

### 5.1 Where it fires in the tick

The tick of [`app-host.md`](app-host.md) section 3 gains one step between layout (step 4) and
sync (step 5). Notation: `round` counts settle rounds within one tick.

```text
3. tick:        scheduler.flush(); animations; collect dirt          (FramePipeline::tick)
4. style+layout: resolve, layout, apply_layout
4a. publish:    read_model.publish(result, ++generation)
4b. observe:    round = 1
                loop:
                  due = observers.collect(read_model, depth_floor)   // size changed since last delivery
                  if due is empty: break
                  observers.deliver(due)                              // app callbacks; may call setters
                  if not scheduler.pending(): break                   // nothing changed state
                  if round == max_rounds: defer(remaining); break     // counted, wake next tick
                  scheduler.flush(); style; layout; apply_layout; publish
                  depth_floor = shallowest depth delivered this round
                  round += 1
5. sync_style, 6. damage, 7. paint ...                                 (unchanged)
```

- **Never mid-layout.** Callbacks run only after `apply_layout` wrote bounds and the read model
  is published. Yoga is not re-entered from a callback; a setter only marks a ticket.
- **Paint sees the settled state.** Damage and paint run after the loop, so a reaction that
  changes layout in the same tick never shows a stale frame (the flash of option C).
- **Render frames between ticks never deliver.** Interpolation is paint-only
  ([`game-loop.md`](game-loop.md) section 3.1) and does not change layout, so nothing to observe.

### 5.2 Coalescing and change detection

- One delivery per observation per tick at most, carrying the latest size. Multiple layout runs
  in one tick (settle rounds) still deliver an observation at most once per round, and a node
  whose size returns to its last delivered value is not delivered.
- "Changed" compares the observed box (section 5.5) with the last delivered value using exact
  float equality. Layout is deterministic, so equal inputs give bit-equal results; no epsilon.
- Position-only moves do not deliver. Size is what callbacks branch on; position consumers use
  overlay anchors, which already track anchor layout at the frame boundary
  ([`overlays-and-layers-v1.md`](../specs/overlays-and-layers-v1.md)), or read the query.

### 5.3 Feedback-loop prevention

A callback that sets state that changes the observed size is the classic loop. Three bounds:

1. **Depth rule** (browser 2.1). In round `r > 1` only observations strictly deeper than the
   shallowest node delivered in round `r - 1` are delivered; shallower ones are deferred. Each
   round therefore strictly increases the depth floor, so the rounds terminate by tree depth even
   without a budget.
2. **Round budget**: `max_rounds` (default 4, runtime-config key `layoutObservationMaxRounds`,
   range 1..16). `1` is option C exactly. Each round also runs `UpdateScheduler::flush()`, which
   keeps its own pass budget (`kDefaultMaxPasses = 8`), so the worst case per tick is
   `max_rounds` layouts and `max_rounds * 8` update passes.
3. **Deferral, not drop.** Deferred observations stay due; the loop is woken
   (`RuntimeLoop::request_frame()`), and they are collected first on the next tick. Correctness
   wins over idling: a stale size must not stay on screen. An app whose callbacks oscillate
   (A grows B, B shrinks A) therefore keeps the loop awake. That is an app bug; it is made
   visible, not hidden:
   - counters in the frame diagnostics (`src/runtime/diagnostics.cpp`):
     `layout_observation_deliveries`, `layout_observation_rounds`,
     `layout_observation_deferred`, `layout_observation_round_limit_hits`;
   - a structured diagnostic `TESSERA_LAYOUT_OBSERVER_LOOP` (warning) once per observation that is
     deferred on `kLoopReportTicks` (default 8) consecutive ticks, naming the node's `uiId`.

Equality-gated setters (section 4 item 6) and derived predicates (`e.width >= 600`) make most
callbacks idempotent, so in practice round 2 is rare.

### 5.4 Interaction with container queries

Container queries are the same loop with an engine-owned callback: after layout, a container whose
query-relevant size crossed a condition marks its descendants style-dirty, and the settle round
re-resolves style and layout. With `container-type: inline-size` containment the container's own
width does not depend on descendants, so one extra round settles it. Container-query re-evaluation
runs before app observations in each round, so app callbacks see sizes that already reflect the
container styles.

### 5.5 What is observed

| Field | Meaning |
|---|---|
| `width`, `height` | Border box (the scene `Node::bounds()` size; ADR-018 keeps scene bounds authoritative) |
| `content_width`, `content_height` | Border box minus border and padding, as resolved by Yoga |
| units | Logical pixels (dp), unsnapped. Pixel snapping is paint-time only (`pixel_snap.cpp`) |
| `dpi_scale` | Carried on the C++ `LayoutInfo` only; a DPI change with the same logical size does not deliver |

- Change detection uses the border box **and** the content box: either changing delivers.
  Browser `ResizeObserver` makes the box a per-observation choice; one combined trigger is simpler
  and the payload carries both, so a choice adds nothing.
- Content insets are not stored on `Node`. `LayoutBox` (`layout_engine.hpp`) gains four floats
  (`content_left/top/right/bottom` from `YGNodeLayoutGetPadding` + `YGNodeLayoutGetBorder`); the
  cost is per laid-out box in the transient `LayoutResult`, not per scene node.
- Transforms do not change layout size, so a scale animation never delivers (as in browsers).
  A transition on a layout property (`height`) changes layout each tick it runs and therefore
  delivers once per tick while it runs; that is the documented tick-rate ceiling, not a
  per-render-frame callback.

### 5.6 Initial size, display none, unmount

- **Initial.** A new observation's last delivered size is "none", so the first layout that
  includes the node delivers, including 0x0 (unlike browsers, where 0x0 is skipped).
- **No box.** A node without a layout box (not in the result, for example `display: none`)
  delivers 0x0 once on the transition, then nothing until it gets a box again.
- **Lifetime.** An observation is attached at mount and detached at unmount, exactly like
  `UpdateTicket`: generated `mount()`/`unmount()` do it; developer C++ uses a move-only RAII
  `LayoutObservation` that detaches in its destructor. Detach during delivery is safe (the entry is
  tombstoned and compacted after the round); an observation attached during delivery is first
  collected in the next round.
- **Debug guard.** Detaching is required before the node dies. A debug-only check in
  `LayoutObserverSet::collect` asserts the stored `NodeId` still matches the node pointer's id.

### 5.7 Storage and cost when nothing observes

- **No change to `Node`.** No inline table, no new bit. The open node-memory work (PR #24,
  `kNodeSizeBudget = 320`, not yet on `main`) is unaffected either way.
- `LayoutObserverSet` is a per-window side table: a `std::vector<LayoutObservation*>` in attach
  order (mount order, parent before child, the same order `UpdateScheduler` uses), plus a reserved
  `due` scratch vector. `reserve(n)` sizes both; attach outside reserved capacity may allocate,
  but attach happens at mount, never on the frame path. `collect` and `deliver` do not allocate.
- Each observation is a plain struct: function pointer + context (no `std::function`, no RTTI),
  `Node*`, `NodeId`, cached depth, last delivered box, deferred-tick count.
- Cost: an empty set returns from `collect` immediately (one size check). With `k` observations,
  `collect` is O(k) per layout run and nothing on ticks without layout. A static UI does not lay
  out, so it does not collect, deliver, or present.
- `LayoutReadModel::publish` builds an id-to-index table over `LayoutResult::boxes`, reusing its
  capacity (allocation only when the tree grows). It is skipped entirely when no read model is
  installed.

### 5.8 Threading and determinism

- UI thread only: attach, detach, collect, deliver and every query assert the owner thread in debug
  builds, like `UpdateTicket::schedule()`. Other threads never read layout; a telemetry thread gets
  sizes through a callback that copies the values into its own bounded queue.
- Order is attach order within a round, rounds in sequence; no wall clock, no hash order. The same
  input script under `ManualClock` yields the same deliveries, rounds and counters.

### 5.9 C++ runtime surface

```cpp
namespace tessera::ui::layout {

struct LayoutInfo {             // value snapshot, logical px
    rhi::Rect border_box;       // root-relative, untransformed (from LayoutResult)
    rhi::Rect content_box;      // root-relative
    rhi::Rect visual_bounds;    // Node::world_bounds(): transform and scroll applied
    float dpi_scale = 1.0f;
    std::uint64_t generation = 0; // layout generation that produced it
};

class LayoutReadModel {         // query side; never runs layout
public:
    void publish(const LayoutResult& result, float dpi_scale);   // host, after apply_layout
    [[nodiscard]] std::optional<LayoutInfo> find(const Node& node) const;
    [[nodiscard]] std::uint64_t generation() const;
};

struct SizeChange { float width, height, content_width, content_height; };

class LayoutObservation {       // move-only; detaches in its destructor
public:
    using Fn = void (*)(void* context, const SizeChange& size);
    LayoutObservation(Node& node, Fn fn, void* context);
};

class LayoutObserverSet {       // notification side, one per window
public:
    explicit LayoutObserverSet(std::uint32_t max_rounds = 4);
    void reserve(std::size_t observations);
    void attach(LayoutObservation& observation);
    void detach(LayoutObservation& observation);
    // Host steps 4b: collect due observations, deliver, report whether state was dirtied.
    LayoutObserveStats observe_round(const LayoutReadModel& model, UpdateScheduler& scheduler);
    [[nodiscard]] bool deferred() const;
};

} // namespace tessera::ui::layout
```

A developer handler that positions a popover reads `read_model.find(*ref_box_)`; that is a query
of committed state, so a write followed by a read in the same handler sees the previous layout
(documented, and the opposite of DOM forced layout).

## 6. TSX surface

### 6.1 `onSizeChange`

```tsx
const [wide, setWide] = useState(false);
<View className="panel" onSizeChange={(e) => setWide(e.width >= 600)}>
  {wide ? <TwoColumns /> : <OneColumn />}
</View>
```

- New event prop on every intrinsic that produces a layout box (`View`, `Text`, `Button`,
  `TextInput`, `TextArea`, `Image`, `Path`, `Scroll`, `CustomElement`, and the containers of
  `VirtualList`/`VirtualGrid`); rejected on `Spacer`, `ErrorBoundary`, `Slot` and `Component` nodes
  (a component has no box of its own; observe its root inside the component).
- New payload record `SizeChangeEvent`: `width: float`, `height: float`, `contentWidth: float`,
  `contentHeight: float` (logical px). The whole record is the primary value.
- All three handler forms of [`jsx-subset-v1.md`](../specs/jsx-subset-v1.md) section 9 apply:
  inline setter handler (the common case above), native handler (`// Note: bind::<id>`) for C++
  telemetry or imperative work, and a setter reference when the state type is `SizeChangeEvent`.
- The event does not bubble and has no capture phase; it is not dispatched through
  `dispatch_event`. It is delivered by `LayoutObserverSet`.

### 6.2 Manifest and IR

- **Intrinsics manifest**: one event row in
  [`common-and-events.md`](../specs/jsx-intrinsics-v1/common-and-events.md) section 2 and in
  [`machine-readable.md`](../specs/jsx-intrinsics-v1/machine-readable.md); the typings generator
  emits `SizeChangeEvent` and `onSizeChange?: (e: SizeChangeEvent) => void`.
- **IR**: no new field. The node's existing `events` map gains `"onSizeChange": "handler::<n>"`
  or `"bind::<id>"`. Adding an event name is additive, so `irVersion` stays 1
  ([`ir-v1.md`](../specs/ir-v1.md) section 12). The validator rejects it on box-less kinds.
- **Codegen** (consumes IR only): per observed node, a `tessera::ui::layout::LayoutObservation`
  member and a static thunk `on_size_change_<n>(void*, const SizeChange&)` that converts to
  `SizeChangeEvent` and runs the handler; `attach_layout_observers(LayoutObserverSet&)` /
  `detach_layout_observers(...)` mirror `attach_scheduler`. Inside a list loop or a virtual item
  template, each realized row owns its observation in the row table. A merger idempotency test
  covers the new member and thunks.
- **Designer**: the Wasm evaluator runs inline setter handlers after its own layout with the same
  round rule, so previews match production ([ADR-037](../../decisions/ADR-037-self-contained-wasm-designer.md)).

### 6.3 Intrinsic sizing for U1

```tsx
<TextArea value={notes} onChange={(e) => setNotes(e.target.value)}
          style={{ fieldSizing: "content", minHeight: 48, maxHeight: 240 }} />
```

`field-sizing: content` (CSS Forms; `fieldSizing` in the style object) makes the `TextArea` a
measured Yoga leaf: its height is the wrapped text height plus padding, clamped by `min-height`
and `max-height`, and it scrolls internally above the maximum. `rows` stays the default height
for `field-sizing: fixed` (the default). This is one layout pass with no callback and no loop,
and it is what browsers ship for the same need.

### 6.4 Why no `useElementSize(ref)` hook

The hook manifest only knows `environment` lowering without parameters (subset section 7.1), and
react-compat Q6 decided refs have no members in V1. A hook would need parameters, a new lowering
kind and per-ref state, and it would hide the same settle loop behind an unfamiliar timing model.
`onSizeChange` plus `useState` gives the same result with existing machinery. Revisit if real
apps repeat the same `onSizeChange`/`useState` pair often enough to want sugar.

## 7. CQRS evaluation

CQRS separates commands (state changes) from queries (reads of a projection). Mapped onto Tessera:

| CQRS idea | Tessera today / proposed | Verdict |
|---|---|---|
| Commands | Generated `set_<state>` setters; the IR handler `actions` list is literally a command list | Already there |
| Command dispatch | Event handlers; the `UpdateScheduler` batches commands into one apply per tick | Already there |
| Cross-thread command channel | The async completion queue, drained by `RuntimeLoop`; setters run in the completion | Already there; no second queue |
| Read model / projection | `LayoutReadModel`: committed layout, versioned by generation, updated at one defined point | **Adopt** (section 5.9) |
| Queries never cause writes | `find()` never forces layout | **Adopt**; this is the DOM-thrashing lesson |
| Idempotent commands | Equality-gated setters | **Adopt**; small and removes most redundant work |
| Derived values / selectors | JSX pure expressions are re-evaluated in `apply_state` and only mark Paint when changed; `e.width >= 600` is a selector | No new helper needed |
| Command objects, bus, mediator | - | Reject: allocations and indirection for no new capability |
| Event sourcing, separate state stores | - | Reject: Tessera state is per-component and in place |

So CQRS fits as vocabulary and earns three concrete things: the layout read model, the
"queries never write" rule, and idempotent (equality-gated) setters. Everything that would add a
command type hierarchy or a bus is over-engineering for an in-process, single-threaded UI.

Equality-gated setters, precisely: for a state type with `operator==` (every subset type: numbers,
bool, string, records, arrays), `set_x(v)` returns without `schedule_update()` when `v == state_x_`.
This changes observable behavior only in that `update()` no longer runs for no-op writes; effects
already compare dependencies, so no effect runs differently.

## 8. Test strategy

All headless, `ManualClock`, null paint sink, the stub text measurer:

- `layout_read_model`: publish/find, generation increments, missing node, content insets.
- `layout_observer`: initial delivery (including 0x0), one delivery per tick under repeated
  layouts, no delivery for position-only moves or transform changes, display-none transition,
  detach during delivery, attach during delivery, attach-order determinism, zero work when empty
  (counter-based), no allocation in `collect`/`deliver` after `reserve` (allocation-counting hook
  as in `virtual_allocations`).
- `layout_observer_settle`: a callback that grows a child settles in round 2 with one paint; the
  depth rule defers a shallower re-dirty; an oscillating pair hits the round budget, is deferred,
  wakes the loop, and reports `TESSERA_LAYOUT_OBSERVER_LOOP` after `kLoopReportTicks`;
  `max_rounds = 1` behaves as next-tick delivery.
- `codegen_size_change`: front-end acceptance/rejection per intrinsic, IR shape, goldens,
  merger idempotency; `codegen_size_change_runtime` drives a generated panel across 600 px.
- `setter_equality`: no scheduled update for equal writes; effects unchanged.
- Golden: a `field-sizing: content` `TextArea` at 1, 3 and 20 lines (lavapipe/CPU only).

## 9. Implementation plan

Each slice is behavior + tests + docs and leaves `main` green. Dependency order:

1. **Content insets in `LayoutBox`** (Yoga padding + border) and `LayoutReadModel`
   (`publish`, `find`, generation). Tests `layout_read_model`. Unblocks U3/U5 for C++.
   **Done:** `LayoutBox::content_insets`, `ui/layout/layout_read_model.hpp`, test `layout_read_model`.
2. **`LayoutObserverSet` + `LayoutObservation`** (attach/detach, collect, deliver, initial,
   coalescing, depth rule bookkeeping) without settle rounds. Tests `layout_observer`.
   **Done:** `ui/layout/layout_observer.hpp` (`LayoutObservation`, `LayoutObserverSet`: attach/detach,
   `collect(model, min_depth)`, `deliver`), test `layout_observer`.
3. **Settle rounds** in a host-facing helper (`observe_and_settle`) that takes the scheduler and
   a relayout callback, plus the runtime-config key, counters and the loop diagnostic. Tests
   `layout_observer_settle`. Wire into the app host when [`app-host.md`](app-host.md) lands; until
   then the helper is exercised by the test harness.
4. **Equality-gated setters** in codegen (independent; can land any time). Tests
   `setter_equality`, golden updates.
5. **`onSizeChange` front end + IR validator + typings**, then **codegen + merger** (two commits).
   Tests `codegen_size_change*`.
6. **Designer evaluator** delivery after preview layout.
7. **`field-sizing: content` for `TextArea`**: property registry, Yoga measure for the editor
   text, internal scroll above max. Golden.
8. **Container queries**: `container-type`/`container-name`, `@container` evaluation as an
   engine observer inside the settle round; update `stylesheet-v1.md` and `responsive.md`.

## 10. Owner decisions

Decided 2026-11-11: the owner accepted every recommendation below as written (event name
`onSizeChange`, same-tick settle with 4 rounds, keep waking plus diagnostic on oscillation, size
only, observer before container queries, equality-gated setters by default, `field-sizing: content`).
The table is kept as the record of the options considered.

| # | Question | Options | Recommendation |
|---|---|---|---|
| 1 | Event name | `onSizeChange`; `onResize` (reads like `ResizeObserver` but collides with window resize vocabulary); `onLayout` (React Native name, but RN fires on position too) | `onSizeChange` |
| 2 | Same-tick settle vs next-tick | Rounds default 4; or default 1 (next tick, simpler, one-frame flash) | Default 4, configurable |
| 3 | Oscillation policy | Keep waking (correct, burns power, diagnostic); or stop delivering after N ticks (idles, may leave stale UI) | Keep waking + diagnostic; debug builds may assert |
| 4 | Position observation | Size only; or add `onLayoutChange` with position | Size only; overlays anchor, reads use the query |
| 5 | Sequencing vs register entry #4 | Observer before container queries; or wait for container queries | Observer first (sections 4 and 5.4) |
| 6 | Equality-gated setters by default | Always; or opt-in per state | Always; no known caller relies on no-op updates |
| 7 | `field-sizing` spelling | Standard `field-sizing: content`; or a `TextArea` prop `autoGrow` | Standard CSS property; React users can write it in `style` |
