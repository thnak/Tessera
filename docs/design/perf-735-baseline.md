# Issue #735 baseline — measured 2026-11-21 at `9febdf3c`

`frame_alloc_probe`, 20 warm-up / 40 measured frames. Windows = MSVC Release (`vs2026-release`),
Linux = GCC Release (`ci`) on `thnak-server`.

## 1,501 nodes (500 cards)

| scenario | Win ms/frame | Linux ms/frame | Win allocs/frame | Linux allocs/frame |
|---|---|---|---|---|
| idle | 0.000 | 0.000 | 3 | 0 |
| paint-dirty | 0.474 | 0.883 | 65.5 | 57.5 |
| text-change | 0.733 | 0.881 | 65.5 | 55.5 |
| class-toggle | 1.113 | 1.097 | 3426.0 | 3412.5 |
| layout-dirty | 1.699 | 1.175 | 5015.0 | 5012.0 |
| size-change | 17.951 | 9.769 | 8125.3 | 8090.4 |

## 6,001 nodes (2000 cards)

| scenario | Win ms/frame | Linux ms/frame | Win allocs/frame | Linux allocs/frame |
|---|---|---|---|---|
| idle | 0.001 | 0.000 | 3 | 0 |
| paint-dirty | 3.584 | 2.535 | 65.5 | 57.5 |
| text-change | 3.455 | 2.538 | 65.5 | 55.5 |
| class-toggle | 6.123 | 4.485 | 13178.0 | 13164.5 |
| layout-dirty | 15.987 | 5.585 | 20017.0 | 20014.0 |
| size-change | 98.022 | 49.951 | 130960.3 | 115285.4 |

## Notes

- `needs_layout=0/40` on class-toggle **and** size-change: neither marks `Dirty::Layout`, so the
  layout-unchanged gate (slice A) is the only thing that can skip the layout pass for them.
- Windows is the slower lane on size-change (18.0 vs 9.8 ms at 1.5k; 98.0 vs 50.0 ms at 6k), so the
  V1 one-frame size-change gate is a Windows problem first.
- `class-toggle` allocates ~3.4k/frame at 1.5k nodes and ~13.2k/frame at 6k — the whole-tree
  `build_layout_input` + signature + `index_nodes` rebuild, which A/B/H target.
