# ADR-056: Generated components update only what changed: dependency masks, and children are scheduled, never called

- Status: Accepted (2026-11-17; owner direction in issue #440: pick the best fix, large changes accepted
  while the framework is new; owner confirmed the proposal and slices "yes")
- Date: 2026-11-17
- Related: [observable.md](../docs/design/observable.md) section 7 (the measurement this decision
  answers), [component-model-v1.md](../docs/specs/component-model-v1.md) section 2 (hooks and
  batching), [dirty-rendering.md](../docs/design/dirty-rendering.md),
  `include/tessera/ui/scene/update_scheduler.hpp`, `src/compiler/codegen/src/{emit_lifecycle,emit_schedule,interpolation}.cpp`,
  [ADR-040](ADR-040-generated-base-and-developer-class.md) (the generated base this changes),
  GitHub issue #440

## Context

State writes are already coalesced: a setter marks the component's `UpdateTicket`, and
`UpdateScheduler::flush()` runs each queued component's `update()` once per tick, so one click gives
one flush and one frame. What is not coalesced is the work inside and between components.
`codegen_update_fanout` (issue #440) measured it on the committed `Page`/`Row` bases, identically at
n = 10, 100 and 1000:

| Scenario | scheduler tickets | `update()` calls made by an owner | `Row` runs total |
|---|---|---|---|
| a child's own state changes | 1 | 0 | 1 |
| the owner's state changes, and feeds no child | 1 | n + 1 | n + 1 |
| the owner and a child are both dirty | 2 | n + 1 | n + 2 |

Two mechanisms cause the cost:

1. **No dependency tracking.** A generated `update()` is `update_effects(); apply_state();` and then
   `update()` on every static child and every live per-item instance. `apply_state()` re-evaluates
   every dynamic binding of the component and relies on a per-binding hash gate to avoid writing
   unchanged nodes. The output stays small, but the CPU is O(bindings) per component and O(n) per
   owner change, for state the children never read.
2. **Two ways to run one update.** Each component attaches its own ticket, and its parent also calls
   its `update()` directly. A direct call does not clear the ticket's queued mark, so a child that is
   dirty at the same time as its parent runs twice.

Nothing here is a correctness bug: the hash gate makes the extra runs no-ops on the tree. It is a
cost that grows with list size, in the default shape of React-style code (one parent array in state).

## Decision

1. **Every binding has a compile-time dependency set.** The compiler already knows which state cells
   and props each text, metadata, style and child-set expression reads (the IR names them). Codegen
   assigns each state cell and each prop one bit in the component's **dirty mask** and records, per
   binding, the mask of bits it reads.
2. **Setters set a bit and queue the ticket.** A setter that changes a value (equality gating is
   unchanged) sets that cell's bit and schedules the component. `apply_state()` re-evaluates a
   binding only when its dependency mask intersects the dirty mask, then clears the mask. A component
   with no dirty bit does no binding work. The hash gate stays as the second filter, so a recomputed
   binding that produced the same value still writes nothing.
3. **A parent never calls a child's `update()`.** The parent's `apply_props()` writes a child's props
   when the prop's hash changed and, only then, sets that prop's bit on the child and schedules the
   child's own ticket. The scheduler is the single place a component's `update()` runs.
4. **Order and pass guarantee.** Tickets run in attach order, which is mount order, so a parent runs
   before its children. A child scheduled by its parent's `update()` is later in the same pass and
   runs in that pass, so scheduling a child does not cost a second pass. The pass limit
   (`kDefaultMaxPasses`) and the cycle report are unchanged.
5. **Lists and conditionals.** A list's items depend on the list's state cell as a whole: a change to
   it re-evaluates the list's children and per-item content, which is real work. A change to unrelated
   state does not. A conditional child set depends on the cells its condition reads. Per-item
   instances are scheduled like any child, by prop change.
6. **Wider than 64 cells.** The mask is `std::uint64_t` for up to 64 cells and an array of words above
   that; the generated code does not change shape, only the mask type.
7. **Unchanged by this decision:** `useEffect` dependency semantics (already exact), animated-value
   ticks and the reduced-motion hook (they set their cell's bit through the same setter path),
   `update()` staying public and callable by hand for headless tests, and the single-thread rule.

## Consequences

- **Result, measured by the same test.** An owner change that feeds no child runs 1 ticket and no
  child update; an owner and child both dirty run once each; a child's own change is unchanged. The
  fan-out test's expected numbers are rewritten and become the regression gate.
- **A list change is still O(n).** That is the list's real dependency, not waste. Large lists remain
  the job of virtual collections.
- **Every committed generated golden changes once** (about two dozen files), in the slice that
  changes the emitted shape. The generator is the only place that changes; developer classes do not.
- **Developer classes that called `update()` on a child by hand** keep working (it stays public) but
  should not; the supported way is a setter or a prop.
- **A new class of bug becomes possible:** a missing dependency bit means a stale binding. The
  mitigation is that the bits come from the IR, not from hand-written code, and each slice carries a
  test per binding kind that mutates one input at a time and asserts the exact bindings re-run.
- It adds no dependency, no allocation on the frame path (the mask is a member), and no JavaScript.

## Alternatives

- **Skip a child whose props hash did not change, but keep direct calls.** Removes part of the
  fan-out but not the double run, and every binding in a child that does update is still re-run.
- **Leave it and document "keep state per item".** Cheap, but it makes the costly shape the default
  and puts the burden on every app author.
- **Runtime dependency tracking (signals, read-tracking proxies).** Fine-grained without compile-time
  analysis, but it needs hidden allocation and indirection on the update path and a tracking context,
  against the no-hidden-allocation rule; the compiler already has the information statically.
- **Virtual DOM style diffing.** Rejected for the reason ADR-040's generated bases exist: there is no
  runtime tree to diff, the generated code writes nodes directly.

## Follow-up

Three slices, each green on its own (issues linked from #440):

1. Dependency analysis in codegen: compute each binding's read set from the IR. No emitted change, no
   golden churn; unit-tested over IR fixtures.
2. Emit the dirty mask: setters set bits, `apply_state()` skips bindings with no dirty dependency.
   Goldens regenerate. Per-component cost drops; the owner's fan-out is unchanged.
3. Children are scheduled, not called: `apply_props()` schedules a child on a prop change and the
   direct `update()` calls are removed from the generated `update()`. Goldens regenerate and
   `codegen_update_fanout` is rewritten to the numbers in Consequences.
