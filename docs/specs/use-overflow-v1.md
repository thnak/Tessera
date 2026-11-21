# `useOverflow` v1

Status: draft, normative once accepted. Specifies the `useOverflow` behavior hook of
[ADR-064](../../decisions/ADR-064-astryx-style-component-system.md) item 7 (issue #634). The
behavior-hook machinery it stands on is [`behavior-hooks-v1.md`](behavior-hooks-v1.md); this file
adds only what is specific to this hook.

Terms: the **target** is the row the hook's `ref` names; an **item** is one of its children; the
**read model** is `layout::LayoutReadModel`, which holds every node's box from the last layout pass;
the **reserved slot** is the width a trailing control (a "more" button) needs.

## 1. Call form

```tsx
const rowRef = useRef<View>(null);
const overflow = useOverflow(rowRef, { reserved: 96, gap: 8 });
return <View ref={rowRef}>{items.slice(0, overflow.count)}</View>;
```

One `const` binding at the top level of the component body, under the placement and order rules of
every hook. The target is required; the options object is optional because every option has a
default.

## 2. Arguments

| Argument | Type | Default | Meaning |
|---|---|---|---|
| `target` | a `useRef` of any element | required | the row whose children are measured |
| `reserved` | `number` | `0` | the width the trailing slot needs, in logical pixels |
| `gap` | `number` | `0` | the gap between items, in logical pixels |

## 3. Outputs

| Output | Type | Meaning |
|---|---|---|
| `count` | `number` | how many leading children fit |
| `overflowing` | `boolean` | whether any child does not fit |

## 4. Rules

Each rule is normative and has a test in `tests/unit/overflow_tests.cpp`.

1. **The count is the number of leading children that fit.** Children are walked in tree order and
   accumulated until the next one would exceed the available width.
2. **The available width is the target's content box.** Padding and border are already excluded by
   the read model's `content_box`.
3. **The reserved slot is subtracted first.** A 300 px row with a 100 px trailing control has 200 px
   for items, so the count is the number that fit *alongside* the control.
4. **The gap is counted between items, not after the last.** `n` items use `n-1` gaps.
5. **An unmeasured child stops the walk.** A child the last pass did not measure (added since) cannot
   be counted, because counting it would claim a width nobody measured.
6. **No measurement reports nothing fits.** With no read model, or a target the model does not know,
   `count` is 0 and `overflowing` is true: a component renders its narrow layout for the first frame
   rather than claiming everything fits.
7. **An update recomputes.** Changing `reserved` or `gap` re-measures in the same call.
8. **Detach resets.** Detaching clears both outputs to their manifest defaults.
9. **An unresolved target attaches nothing.** When the target ref holds no node, the behavior
   attaches nothing.
10. **The behavior never forces a layout.** It reads the boxes the last pass produced; it does not
    call the layout engine, poll, or schedule a frame. A component that renders `count` children
    settles in one pass because the widths it measures are the widths the previous pass committed.
11. **No hidden global state.** The behavior holds one target pointer, one model pointer and two
    numbers. It registers no listener and reads no clock.

## 5. Accessibility

- A row that hides items must keep them reachable: the trailing control that reveals them needs an
  accessible name and a role, and the hidden items must be in a menu or a second row rather than
  simply absent. The hook reports a count; it does not decide where the rest go.
- Rule 6 matters for a screen reader as much as for a sighted user: a first frame that claims every
  item fits would announce a list the user cannot see.
- The hook does not set `aria-hidden` on the hidden items; the component that renders only `count`
  children never creates them, which is the correct answer.

## 6. States

| State | `count` | `overflowing` |
|---|---|---|
| detached | `0` | `false` |
| no measurement | `0` | `true` |
| all items fit | the child count | `false` |
| some items fit | the cut index | `true` |

## 7. Verification map

| Rule | Test |
|---|---|
| 1 | `test_cut_index` |
| 2 | `test_all_items_fit` |
| 3 | `test_reserved_slot` |
| 4 | `test_gap_is_counted` |
| 5 | `test_unmeasured_child_stops_the_walk` |
| 6 | `test_no_model_reports_nothing_fits` |
| 7 | `test_update_recomputes` |
| 8 | `test_detach_resets` |
| 9 | `test_missing_target_attaches_nothing` |
| 10 | the whole file: every test publishes a layout by hand and asserts the count arithmetically |
| 11 | the whole file: every test builds its own scene and asserts no shared state |

## 8. What this hook does not do

- It does not render the trailing control; the component does.
- It does not decide which items to hide beyond "the leading ones that fit". A component that wants
  a different policy (hide the middle, keep the last) reads `count` and slices itself.
- It does not measure a vertical column: the walk is on widths, because the hook's consumers
  (OverflowList, Toolbar, Breadcrumbs) are rows.
- It does not re-measure on a resize by itself: the host re-runs layout, and the component's next
  update calls `measure` through the normal update path.
