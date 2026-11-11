# ADR-041: Layout observation, a layout read model, and container queries second

- Status: Proposed
- Date: 2026-11-11
- Revises (when accepted): the open-questions register entry `design/window-resize.md#4` ("no
  public `onResize`/`ResizeObserver` until container queries ship"). [ADR-020](ADR-020-window-resize-and-lifecycle.md)
  itself is not superseded: its section 1 stays in force.
- Related: [ADR-005](ADR-005-yoga-layout-adapter.md) (Yoga layout),
  [ADR-016](ADR-016-stylesheet-engine.md) (stylesheet engine),
  [ADR-017](ADR-017-input-and-fixed-step-runtime.md) (fixed-step runtime),
  [ADR-018](ADR-018-dirty-rendering-and-frame-pacing.md) (dirty rendering; scene bounds
  authoritative), [ADR-036](ADR-036-own-jsx-frontend-and-react-like-model.md) (React-like model),
  [ADR-037](ADR-037-self-contained-wasm-designer.md) (Wasm designer). Design note:
  [`layout-observation.md`](../docs/design/layout-observation.md).

## Context

Components cannot learn their laid-out size. The register entry above deferred any observer
because an OS-cadence resize callback would leak platform behavior into components (ADR-020
section 1). Apps still need a growing text area, a panel that adapts to its own width, a
post-layout box for popovers and scroll-into-view, and size telemetry. Container queries are
parsed but not evaluated and cannot switch behavior or structure. Generated setters now schedule
updates through `UpdateScheduler` (issue #5), which gives a well-defined place to apply reactions.

## Decision

1. **Layout read model.** After `apply_layout` the host publishes the committed layout into a
   `LayoutReadModel`; `find(node)` returns a `LayoutInfo` value (border box, content box, visual
   bounds, DPI scale, generation). A read never runs layout.
2. **Size observations.** A per-window `LayoutObserverSet` delivers a `SizeChange` to each
   observation whose border or content box changed since its last delivery, after layout and
   before damage/paint, once per tick at most, including an initial delivery. It is driven by
   Tessera's deterministic tick, never by OS events, so ADR-020 section 1 holds.
3. **Bounded settle rounds.** When callbacks dirty components, the host re-runs scheduler flush,
   style and layout in the same tick, delivering only to deeper nodes each round (browser depth
   rule), up to `layoutObservationMaxRounds` (default 4; 1 means next-tick delivery). Leftovers are
   deferred to the next tick with a wake, counted, and reported as `TESSERA_LAYOUT_OBSERVER_LOOP`
   when they persist.
4. **Storage.** No change to `ui::Node`. Observations live in a side table in mount order; plain
   function pointer + context; no allocation in collect/deliver after `reserve`; zero work when
   nothing observes or nothing was laid out.
5. **TSX surface.** An `onSizeChange` event prop with a `SizeChangeEvent` record, accepting the
   three existing handler forms. It lowers into the existing IR `events` map (`irVersion` stays 1).
   No new hook in V1.
6. **Intrinsic sizing.** `field-sizing: content` on `TextArea` covers the growing text area
   without any callback.
7. **Container queries are the primary answer for styling and ship second**, as an engine-owned
   observer inside the same settle rounds. The observer is the escape hatch for behavior and
   reads, and ships first because container queries need its infrastructure.
8. **Equality-gated setters.** `set_<state>(v)` does not schedule an update when `v` equals the
   current value.

## Consequences

### Positive

- U1-U5 of the design note are covered; DOM-style layout thrashing is impossible (reads are
  snapshots).
- Deterministic and headless-testable; a static UI still idles with zero presents.
- Container queries reuse the same loop instead of inventing a second feedback path.

### Negative / risks

- A tick where callbacks change layout lays out up to `max_rounds` times (Yoga has no partial
  layout).
- An oscillating app keeps the loop awake; the diagnostic makes it visible but does not stop it.
- Reads inside a handler see the previous layout until the next tick or round; this differs from
  the DOM and must be documented.
- The production app host ([`app-host.md`](../docs/design/app-host.md)) is not implemented; the
  settle loop is testable in isolation but reaches production with the host.

## Alternatives considered

- **Keep resize internal until container queries ship.** Rejected: container queries cannot switch
  behavior or answer reads, and they need the same feedback mechanism anyway.
- **Container queries only.** Rejected for the same reason.
- **Next-tick delivery only.** Kept as `max_rounds = 1`; rejected as the default because the
  stale frame is visible.
- **A `useElementSize(ref)` hook.** Rejected for V1: needs hook parameters, a new lowering kind
  and ref members (react-compat Q6 decided none).
- **An observer table inline in `Node`, or a dirty bit hooked into `set_bounds`.** Rejected: it
  grows every node for a rare feature; an O(observers) scan per layout run is cheap and leaves
  `Node` untouched.
- **A CQRS command bus or command objects.** Rejected: setters and the completion queue already
  are the command path; only the read model and idempotent setters earn their place.

## Follow-up

- Slices in [`layout-observation.md`](../docs/design/layout-observation.md) section 9.
- On acceptance: update the register entry `design/window-resize.md#4`, add the event row to
  [`common-and-events.md`](../docs/specs/jsx-intrinsics-v1/common-and-events.md), the
  `layoutObservationMaxRounds` key to [`runtime-config-v1.md`](../docs/specs/runtime-config-v1.md),
  and `field-sizing`/container queries to [`stylesheet-v1.md`](../docs/specs/stylesheet-v1.md).
