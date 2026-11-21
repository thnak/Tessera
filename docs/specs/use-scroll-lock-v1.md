# `useScrollLock` v1

Status: draft, normative once accepted. Specifies the `useScrollLock` behavior hook of
[ADR-064](../../decisions/ADR-064-astryx-style-component-system.md) item 7 (issue #633). The
behavior-hook machinery it stands on is [`behavior-hooks-v1.md`](behavior-hooks-v1.md); this file
adds only what is specific to this hook.

Terms: the **target** is the element the hook's `ref` names; a **scroll container** is a node with a
scroll axis enabled (`Node::scrollable_x`/`scrollable_y`); the **lock** is the state in which the
target is not a scroll container, so a wheel or gesture chains past it.

## 1. Call form

```tsx
const pageRef = useRef<View>(null);
const lock = useScrollLock(pageRef, { active: open });
return <View ref={pageRef}>{...}</View>;
```

One `const` binding at the top level of the component body, under the placement and order rules of
every hook. The target is required; the options object is optional because every option has a
default. The hook is **exclusive** per target: two `useScrollLock` calls on one target are
`TESSERA_BEHAVIOR_DUPLICATE`, because the reference count already expresses "two layers want this
locked" and a second behavior on the same element would double-count one layer.

## 2. Arguments

| Argument | Type | Default | Meaning |
|---|---|---|---|
| `target` | a `useRef` of any element | required | the scroll container to lock |
| `active` | `boolean` | `true` | whether the lock is held |

`active` is reactive: changing it on a later render acquires or releases the lock without
re-creating the behavior.

## 3. Outputs

| Output | Type | Meaning |
|---|---|---|
| `locked` | `boolean` | whether this behavior currently holds a reference |

The output reports **this behavior's** reference, not whether the node is locked overall: a nested
layer that releases its own lock while an outer one still holds one reports `false` and the page
stays locked, which is the truth a component needs.

## 4. Rules

Each rule is normative and has a test in `tests/unit/scroll_lock_tests.cpp`.

1. **Attach locks the target.** When the behavior attaches with `active` true, the target's scroll
   axes are cleared, so it is no longer a scroll container.
2. **The output mirrors the reference.** `lock.locked` is true exactly while this behavior holds a
   reference, and is `false` before the first attach and after detach.
3. **`active` is reactive.** An update that changes `active` acquires or releases the reference in
   the same call, with no re-attach.
4. **The lock is reference-counted.** Two behaviors locking one node hold two references; releasing
   one leaves the node locked, and releasing the last restores it.
5. **The original axes are restored.** The axes the node had before the first lock are what the last
   release restores, not an assumption that both were on.
6. **Detach releases the reference.** Detaching a behavior that holds one releases it, so no lock
   outlives its component.
7. **Detach is idempotent.** A second detach does not release a reference the behavior no longer
   holds.
8. **A destroyed behavior releases its reference.** A behavior destroyed without an explicit detach
   releases its reference in its destructor, so a page cannot stay unscrollable for the session.
9. **An unresolved target attaches nothing.** When the target ref holds no node, the behavior
   attaches nothing and locks no node.
10. **A full table reports the truth.** The lock table is bounded; a behavior that cannot get a slot
    leaves the node unlocked and reports `locked = false` rather than claiming a lock it does not
    hold.
11. **No hidden global state beyond the count.** The only process-wide state is the bounded table of
    `{node, count, axes}` records, which exists because two behaviors cannot otherwise see each
    other's reference. It holds no pointer to a component and is cleared when the last reference is
    released.

## 5. Accessibility

- A scroll lock is a **modal** affordance: it is correct only while a layer owns the interaction. A
  component that locks the page without trapping focus leaves a keyboard user able to Tab into
  content they cannot scroll to, so `useScrollLock` is used with `useFocusTrap`, not instead of it.
- Rule 5 matters for a page that scrolls on one axis only: restoring both axes would make a
  horizontally-clipped page suddenly scroll sideways.
- The lock does not hide content from assistive technology. A component that wants the background
  inert must also set `aria-hidden` or `inert` on it; the hook only stops scrolling.

## 6. States

| State | Target axes | `locked` |
|---|---|---|
| detached | unchanged | `false` |
| inactive | as the node had them | `false` |
| held (one of several) | clear | `true` |
| released, another still holds | clear | `false` |
| released, last reference | restored | `false` |

## 7. Verification map

| Rule | Test |
|---|---|
| 1 | `test_attach_locks_the_target` |
| 2 | `test_attach_locks_the_target`, `test_inactive_lock_marks_nothing` |
| 3 | `test_update_toggles_the_lock` |
| 4 | `test_nested_locks_are_reference_counted` |
| 5 | `test_restores_the_original_axes` |
| 6 | `test_nested_locks_are_reference_counted` |
| 7 | `test_detach_is_idempotent` |
| 8 | `test_destructor_releases_the_lock` |
| 9 | `test_missing_target_attaches_nothing` |
| 10 | evidence gap: the table holds 16 nodes, which no test can exhaust without 16 live scenes; the branch is a guard, not a behaviour a user reaches |
| 11 | the whole file: every test asserts `lock_count` returns to 0 |

## 8. What this hook does not do

- It does not trap focus; that is `useFocusTrap` (section 5).
- It does not hide the background from assistive technology (section 5).
- It does not compensate for a scrollbar disappearing: the engine's scrollbars are overlay
  (`kOverlayScrollbarWidth`), so removing the scroll container does not change layout width.
- It does not block touch overscroll inside the locked area only: clearing the axes makes the whole
  subtree non-scrollable, which is the same result a browser's `overflow: hidden` gives.
