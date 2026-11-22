# ADR-070: A custom frame arena, not `std::pmr`

- Status: Accepted (2026-11-22). Closes `docs/specs/memory-model-v1.md` open question 1.
- Date: 2026-11-22
- Related: issue #701 (memory-model audit, step 2), PR #793 (the probe's site attribution and the
  first fix), `docs/specs/memory-model-v1.md` sections 2, 3, 9 and 11,
  `docs/design/threading-and-memory.md` section 4.
- Terms: a **frame arena** is a block of memory reset at the frame boundary, so everything in it dies
  together; a **high-water mark** is the furthest offset ever handed out, which is what a budget is
  measured against.

## Context

`docs/specs/memory-model-v1.md` section 2 defines a frame arena, a persistent arena and
fixed-capacity pools, and section 9 asks for allocations/frame and arena high-water. None of them
existed in code, and the spec's open question 1 left the *shape* undecided, so nobody could adopt
it.

Step 1 of #701 built the measurement first (`tests/benchmarks/memory/frame_alloc_probe.cpp`, with
`--sites` attribution added in #793). The numbers below are from that probe on `origin/main`
`2ec3ddc3`, Release, GCC 15.2, a 601-node scene, 20 measured frames after 10 warm-up.

| scenario | allocs/frame | dominant sites |
|---|---|---|
| idle | **0** | — |
| paint-dirty | 61 | `parse_color` 372, `bidi_apply_visual_runs` 230, `PaintCache::insert` 284 |
| text-change | 59 | same shape as paint-dirty |
| class-toggle | 182 | `ComputedStyle::set_normalized` 1,080, `CascadeTable::add` 780, `matched_rules` 300 |
| layout-dirty | 202 | one unresolved site at 24,120 (the cascade), `apply_layout` 120 |
| size-change | 600 | the cascade, `collect_retained_boxes` 720 |

Two facts decide this ADR:

1. **A steady-state idle frame already allocates zero.** The spec's headline rule ("no
   `malloc`/`new` in steady-state frame code after warm-up") is *met* today for the idle case, and
   the remaining allocations are all on a *change* path, not a per-frame path.
2. **The remaining sites are not scratch.** `set_normalized` and `CascadeTable::add` build a
   `ComputedStyle` that outlives the frame (it is the node's style for the generation);
   `parse_color` produces a value stored in it; `PaintCache::insert` stores a record that outlives
   the frame by design. A frame arena resets at the frame boundary, so it cannot hold any of them.

## Decision

**A custom arena API, not `std::pmr` wrappers.**

The arena is a small, purpose-built type with an explicit reset point and a high-water counter. It
does **not** expose `std::pmr` types in any public header, and it does not use
`std::pmr::monotonic_buffer_resource` internally.

### Why not `std::pmr`

- **The spec already forbids exposing it.** Section 2: "`std::pmr` may back arenas behind project
  aliases; do not expose `std::pmr` in public APIs." So the choice is only about the *implementation*
  behind the alias, and that is a much smaller decision than the open question implies.
- **`monotonic_buffer_resource` does not give the two things the spec asks for.** It has no
  high-water counter (section 9 requires one) and no fixed-capacity failure mode (section 2 requires
  "pools have fixed capacity and fail with a diagnostic, not silent growth"). Both would have to be
  built on top of it, at which point the wrapper is the arena.
- **It allocates on growth.** `monotonic_buffer_resource` allocates a new upstream block when the
  current one is exhausted and keeps the old one alive until release. A frame arena that must report
  a high-water mark and reset deterministically wants one block it can measure, not a chain.
- **Zero uses today.** `git grep pmr -- include src` returns nothing, so there is no existing
  convention to stay consistent with, and no migration cost either way.

### What the arena is

- One contiguous block, sized from a configured budget, allocated once and reused.
- `reset()` at the frame boundary (spec section 3), which is a pointer store, not a free.
- A high-water counter: the furthest offset ever handed out, reported through frame diagnostics
  (section 9).
- A fixed capacity that **fails with a diagnostic** rather than growing, so a scene that outgrows
  the budget is visible instead of silently slower.
- No destructors run on reset. A type that needs one does not belong in the frame arena, which is
  the same rule `std::pmr::monotonic_buffer_resource` has and the reason the spec pairs the arena
  with "temporary per-frame data, command scratch".

### What it is not for

The measurement above says the arena would fix **none** of the current top sites, because they all
outlive the frame. The arena's first real customer is therefore the *command scratch* the spec names
— the per-frame draw list a sink builds before submission — not the cascade.

That is the honest consequence of measuring first: the arena is worth building for the data it will
hold, not for the numbers in the table above. The cascade allocations are a separate problem
(interning property values, a per-generation style arena that resets with the style generation, not
with the frame), and they are tracked on #701 rather than solved by this decision.

## Consequences

- `docs/specs/memory-model-v1.md` open question 1 is closed; the spec's section 2 rule is unchanged.
- The persistent arena and the pools stay unimplemented until a measured source needs them. The spec
  describes them; this ADR does not commit to building them speculatively.
- A future contributor who reaches for `std::pmr` has this record of why the project did not.
- The counted gate (step 4 of #701) is what keeps the numbers honest: it fails when allocations/frame
  grow, so the arena's effect is measurable rather than asserted.

## References

- `docs/specs/memory-model-v1.md` sections 2, 3, 9, 11.
- `docs/design/threading-and-memory.md` section 4.
- `tests/benchmarks/memory/frame_alloc_probe.cpp` — the measurement, with `--sites`.
- #701 (the audit and its steps), #793 (the probe's site attribution and the first fix).
