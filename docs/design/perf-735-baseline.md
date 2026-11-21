# Issue #735 — measured baselines and slice A results

`frame_alloc_probe`, 50 warm-up / 200 measured frames, median of 5 runs. Windows = MSVC Release
(`vs2026-release`) on an AMD Ryzen 5 9600X; Linux = GCC Release (`ci`) on `thnak-server`.

**Read the Windows numbers as a lower bound.** The local CPU is fast, so a frame that fits here can
still miss on a typical device. The 6k-node and 1.5k size-change targets are gates on the slowest
supported hardware, not on this box.

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

## What slice A does not fix

- **size-change is barely moved.** A real box change must reflow, so the gate correctly declines;
  the remaining cost is `build_layout_input` + Yoga, which is slice D (retained Yoga tree).
- **layout-dirty is unchanged.** A node that marks `Dirty::Layout` is honoured by the gate, so the
  pass still runs. That is the intended contract, not a miss.
- **6k size-change is still ~29 ms on Windows.** Slice D is the fix; the gate cannot help a frame
  that genuinely reflows.
