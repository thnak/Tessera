# Issue #735 — measured baselines and slice A results

`frame_alloc_probe`, 50 warm-up / 200 measured frames, median of 5 runs. Windows = MSVC Release
(`vs2026-release`) on an AMD Ryzen 5 9600X; Linux = GCC Release (`ci`) on `thnak-server`.

**Read the Windows numbers as a lower bound.** The local CPU is fast, so a frame that fits here can
still miss on a typical device. The 6k-node and 1.5k size-change targets are gates on the slowest
supported hardware, not on this box.

## The size-change row was measured wrong until `3f040b47`

The row alternated `width: 121px` / `120px`, but `120px` is the sheet's own value. The "restore"
frame therefore resolved the same computed style as the sheet, the layout-unchanged gate correctly
accepted it, and **half the row was a no-op**. Every size-change number recorded before `3f040b47`
is half the real cost. Both values now differ from the sheet (`121px` / `123px`), and
`layout_gate` reads 0/200 on that row, which is the correct answer for a frame that always moves a
box.

| scenario | nodes | Win before fix | Win after fix | Linux before fix | Linux after fix |
|---|---|---|---|---|---|
| size-change | 1,501 | 4.72 | **9.45** | 9.70 | **18.0** |
| size-change | 6,001 | 32.19 | **59.93** | 52.5 | **92.9** |

Allocations per frame, size-change: 3,981 → 6,290 (1,501 nodes) and 114,813 → 223,453 (6,001) on
Windows. The Linux allocation counts differ slightly (6,252 / 192,138) because `std::string` and
`shared_ptr` sizes differ between the standard libraries.

**The V1 gate is therefore further away than the issue assumed.** A one-frame size change at 1,501
nodes is 9.45 ms on this box and 18.0 ms on the Linux lane, not the 4.79 ms recorded earlier. Slice
D has to remove most of that, not a third of it.

## Baseline at `9febdf3c` (before slice A)

| scenario | nodes | Win ms/frame | Linux ms/frame |
|---|---|---|---|
| class-toggle | 1,501 | 0.340 | 1.097 |
| size-change | 1,501 | 4.790 | 9.769 |
| layout-dirty | 1,501 | 1.699 | 1.175 |
| paint-dirty | 1,501 | 0.474 | 0.883 |
| class-toggle | 6,001 | 6.123 | 4.485 |
| size-change | 6,001 | 30.42 | 49.951 |

Two rows in an earlier single-run pass were cold-cache noise and are **not** reproducible: 17.95 ms
for the 1.5k Windows size-change row (real: 4.79) and 98.0 ms for the 6k row (real: 30.42). The
table above is the honest number, measured as the median of five runs after a warm-up.

## Slice A — the layout-unchanged gate

| scenario | nodes | Win before | Win after | Linux before | Linux after |
|---|---|---|---|---|---|
| class-toggle | 1,501 | 0.340 | **0.189** | 1.097 | **0.784** |
| size-change | 1,501 | 4.790 | **4.525** | 9.769 | 10.055 |
| class-toggle | 6,001 | 6.123 | **1.252** | 4.485 | **2.988** |
| size-change | 6,001 | 30.42 | **29.23** | 49.951 | — |

Allocations per frame, class-toggle at 1,501 nodes: 3,426 → 1,675 (Windows), 3,413 → 1,663 (Linux).
The gate removes the whole-tree `build_layout_input` walk and the signature hash on a frame whose
restyle moved no box.

`layout_gate=20/40` on class-toggle: half the frames are the class change (a layout edit, declined)
and half are the toggle back (also a layout edit). The gate's win on that row is that a *paint-only*
restyle no longer pays for a layout pass at all.

## Slices A+B+G+H together

Measured on the same box, median of 5 runs, 50 warm-up / 200 measured frames:

| scenario | nodes | before | after | allocations before → after |
|---|---|---|---|---|
| class-toggle | 1,501 | 0.340 | **0.070** | 3,426 → 95 |
| class-toggle | 6,001 | 6.123 | **0.250** | 13,178 → 95 |
| layout-dirty | 1,501 | 1.699 | **0.399** | 5,015 → 2,010 |
| layout-dirty | 6,001 | 15.987 | **1.802** | 20,017 → 8,012 |
| paint-dirty | 1,501 | 0.474 | **0.108** | 65 → 22 |
| paint-dirty | 6,001 | 3.584 | **0.375** | 65 → 22 |
| size-change | 1,501 | 4.790 | 6.774 | 8,125 → 3,981 |
| size-change | 6,001 | 30.42 | 36.028 | 130,960 → 114,813 |

The class-change row is the headline: **24× faster at 6k nodes** and 139× fewer allocations. That is
A (no layout pass), B (sync visits only the restyled subtrees), G (dense memo) and H (no map build)
compounding — each removes a whole-tree walk from a frame that changed three nodes.

**size-change regressed slightly** (4.79 → 6.77 ms at 1.5k, 30.4 → 36.0 at 6k, both measured with
the old harness). It is the one row where the gate always declines, so it pays the new digest
comparison on top of a full reflow and gets none of the savings. That is the honest cost of A on the
path A cannot help, and it is why slice D (retained Yoga tree) is the fix for this row rather than a
further gate. Re-measured with the corrected harness, the row is 9.45 ms at 1.5k.

## Slice C — the subtree paint cull

`Node::subtree_paint_bounds()` caches the union of a node's own painted box and every descendant's,
so paint rejects a whole subtree with one test at its root instead of visiting every node in it.

| scenario | nodes | Win before | Win after | Linux before | Linux after |
|---|---|---|---|---|---|
| paint-dirty | 1,501 | 0.108 | **0.026** | 0.223 | **0.057** |
| paint-dirty | 6,001 | 0.375 | **0.083** | 0.880 | **0.191** |
| class-toggle | 1,501 | 0.070 | 0.061 | 0.195 | 0.186 |
| class-toggle | 6,001 | 0.250 | 0.263 | 0.711 | 0.654 |

**paint-dirty is 4.2–4.6× faster on both lanes.** class-toggle is unchanged within noise: the cull
walk was already cheap there after slice H, so this slice's value is paint-dirty, not the
class-change frame the issue predicted.

The cost is `sizeof(Node)` 304 → 320, a real growth (MSVC's layout dump shows all 304 bytes of the
old object occupied, so there was no padding to reclaim). At 10k nodes that is 160 KB.

## The text measure cache cliff

`LayoutEngine`'s text measure cache defaulted to 1,024 entries and is a strict LRU, so a scene with
more distinct text runs than slots evicted the entry it was about to need again. The failure is a
cliff, not a slope, and it sat inside the range this issue targets:

| cards | nodes | labels | allocs/frame |
|---|---|---|---|
| 500 | 1,501 | 1,000 | 6,274 |
| 512 | 1,537 | **1,024** | 6,418 ← capacity |
| 515 | 1,546 | 1,030 | **57,108** ← 8.9×, one card later |
| 2,000 | 6,001 | 4,000 | 223,453 |

The default is now 16,384. At 6,001 nodes that is **223,453 → 24,302 allocations per frame (9.2×
fewer)**, and the curve is linear again (6,274 / 7,475 / 12,279 / 24,286 at 500 / 600 / 1,000 /
2,000 cards).

## What is still not fixed

- **size-change is barely moved.** A real box change must reflow, so the gate correctly declines;
  the remaining cost is `build_layout_input` + Yoga, which is slice D (retained Yoga tree).
- **layout-dirty is unchanged.** A node that marks `Dirty::Layout` is honoured by the gate, so the
  pass still runs. That is the intended contract, not a miss.

## Slice D — the retained Yoga tree

`LayoutEngine::layout` used to build a `YGNode` per `LayoutInput`, apply ~40 computed-style lookups
per node, lay out, and free the whole tree. It now keeps one `YGNode` per node id across frames
(`src/ui/layout/layout_retained.cpp`) and re-applies a node's style only when its
`style::layout_digest` changed, letting Yoga propagate the dirtiness from there. A changed node's
style is rebuilt from a pristine `Style` rather than patched in place, because `apply_style` writes
only the longhands the style *has*: a longhand the author removed would otherwise keep its old Yoga
value. The from-scratch path (`layout_cold.cpp`) stays as the oracle, for `layout_subtree`, and for
grid items.

Measured on the same box as the rows above, median of 5 runs, 50 warm-up / 200 measured frames. The
before column is the from-scratch path measured in the same session, not the `9.45`/`59.93` above;
it reproduces them (9.02 vs 9.45 at 1.5k), so the two are comparable.

| scenario | nodes | before | after | allocations before → after |
|---|---|---|---|---|
| size-change | 1,501 | 9.02 | **0.98** | 6,290 → 2,255 |
| size-change | 6,001 | 62.06 | **8.49** | 223,453 → 107,811 |

The remaining cost is Yoga's own layout pass plus the box walk, which a real reflow cannot avoid.
The acceptance gate is `layout_retained` (`tests/unit/layout_retained_tests.cpp`): a fixed-seed
randomized edit sequence over one tree — size changes, `order` changes, `display: none` and
`display: contents` flips, text changes, a child added and removed, a grid track change, a nested
flex direction flip, and a longhand *removed* — where every frame's retained result must equal
`layout_cold` box for box, including `content_insets` and `text_fit`. It also asserts that a steady
frame re-applies no style, that a viewport change re-applies styles but keeps the tree, and that an
unbounded (negative) viewport axis gates the page adjustments exactly as the from-scratch path does.

The test was verified to have teeth: removing the pristine style reset, the `order` sort, the grid
input refresh, or the child insert each makes it fail on a specific frame. The removed-longhand case
is the one that found a real bug — without the pristine reset a node kept the `width` its author had
just deleted, and the box stayed 128px wide where the from-scratch path produced 38px.

## What is still not fixed after D

- **layout-dirty is unchanged.** A node that marks `Dirty::Layout` is honoured by the gate, so the
  pass still runs. That is the intended contract, not a miss.
- **The remaining size-change cost is Yoga's own pass plus the box walk**, which a real reflow
  cannot avoid. Slices E+F (interned property ids, style as a delta over shared defaults) target the
  style-lookup share of it.
