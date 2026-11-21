# `useFocusTrap` v1

Status: draft, normative once accepted. Specifies the `useFocusTrap` behavior hook of
[ADR-064](../../decisions/ADR-064-astryx-style-component-system.md) item 7 (issue #624). The
behavior-hook machinery it stands on is [`behavior-hooks-v1.md`](behavior-hooks-v1.md); this file
adds only what is specific to this hook.

Terms: the **target** is the element the hook's `ref` names; the **trap** is the engine's modal-focus
state (`Node::set_focus_trap` plus `FocusManager::sync_focus_trap`); the **opener** is the node that
held focus when the trap became active.

## 1. Call form

```tsx
const trapRef = useRef<View>(null);
const [open, setOpen] = useState<boolean>(false);
const trap = useFocusTrap(trapRef, { active: open, restoreFocus: true });
return <View ref={trapRef} aria-modal={open}>{...}</View>;
```

One `const` binding at the top level of the component body, under the placement and order rules of
every hook. The target is required; the options object is optional because every option has a
default. The hook is **exclusive**: two `useFocusTrap` calls on one target are
`TESSERA_BEHAVIOR_DUPLICATE`, because two traps on one subtree have no defined precedence.

## 2. Arguments

| Argument | Type | Default | Meaning |
|---|---|---|---|
| `target` | a `useRef` of any element | required | the subtree the trap confines focus to |
| `active` | `boolean` | `true` | whether the trap is on |
| `restoreFocus` | `boolean` | `true` | whether releasing the trap returns focus to the opener |

`active` is reactive: changing it on a later render turns the trap on or off without re-creating the
behavior. `restoreFocus` is read when the trap is released.

## 3. Outputs

| Output | Type | Meaning |
|---|---|---|
| `active` | `boolean` | whether the trap is currently on |

The output reports the hook's own option, not where focus happens to be. A trap whose subtree has no
focusable node is still active; a component that reads `trap.active` is asking about its own state.

## 4. Rules

Each rule is normative and has a test in `tests/unit/focus_trap_tests.cpp`.

1. **Attach marks the target.** When the behavior attaches with `active` true, the target's trap flag
   is set. With `active` false the flag is left clear.
2. **The output mirrors the option.** `trap.active` equals the `active` option after every attach and
   update, and is `false` before the first attach and after detach.
3. **`active` is reactive.** An update that changes `active` sets or clears the target's trap flag in
   the same call, with no re-attach.
4. **Detach clears what it set.** Detaching a behavior that applied the trap clears the flag, so no
   trap outlives its component. A trap the behavior never applied is not cleared.
5. **Detach is idempotent.** A second detach does not touch a flag the behavior no longer owns.
6. **An unresolved target attaches nothing.** When the target ref holds no node (a conditional branch
   that did not take, an empty list), the behavior attaches nothing and marks no node.
7. **Tab traversal stays inside.** While the trap is active and the host has reconciled the tree, Tab
   and Shift+Tab move among the target's focusable descendants and wrap at both ends instead of
   leaving the subtree.
8. **Nodes outside the trap are not focusable.** While the trap is active, a node outside the target
   subtree is rejected by `FocusManager::allows_focus`.
9. **Releasing restores the opener.** When the trap is released and `restoreFocus` is true, focus
   returns to the node that held it when the trap became active, provided that node is still
   attached, visible and focusable. Otherwise focus stays where it is.
10. **Nested traps release in order.** The last visible trap in tree order is the active one; when it
    is released the next one out becomes active, and focus returns to the node that held it before
    the inner trap.
11. **Escape is not this hook's.** Escape dismissal belongs to the overlay layer
    (`OverlayHost::handle_escape`) and to the component that owns the dialog, not to the trap. A trap
    that also consumed Escape would make a nested popover unable to close itself.
12. **No hidden global state.** The behavior holds one target pointer and one applied flag. It
    registers no listener and reads no clock, so a headless test is deterministic.

## 5. Accessibility

- A trap is only correct on a subtree that is modal. The component that uses it must also set
  `aria-modal="true"` on the target and give it an accessible name; the hook does not set either,
  because a trap on a non-modal subtree would hide the rest of the page from assistive technology
  without saying so.
- Rule 9 is the WCAG 2.4.3 (Focus Order) requirement: a keyboard user who opens a dialog and closes
  it must land back where they were, not at the top of the document.
- Rule 8 is what makes the trap real for a screen reader as well as for Tab: a node that cannot take
  focus is not reachable by sequential navigation either.

## 6. States

| State | `active` | Target trap flag | `trap.active` |
|---|---|---|---|
| detached | n/a | unchanged | `false` |
| inactive | `false` | clear | `false` |
| active | `true` | set | `true` |

## 7. Verification map

| Rule | Test |
|---|---|
| 1 | `test_attach_sets_the_trap`, `test_inactive_trap_marks_nothing` |
| 2 | `test_attach_sets_the_trap`, `test_detach_clears_the_trap` |
| 3 | `test_update_toggles_the_trap` |
| 4 | `test_detach_clears_the_trap` |
| 5 | `test_detach_is_idempotent` |
| 6 | `test_missing_target_attaches_nothing` |
| 7 | `test_tab_traversal_stays_inside` |
| 8 | `test_tab_traversal_stays_inside` |
| 9 | `test_release_restores_focus` |
| 10 | `test_release_restores_focus` (the outer trap is the one restored) |
| 11 | evidence gap: the overlay layer's own tests (`overlay_dismiss`) cover Escape; this hook has no Escape path to test |
| 12 | the whole file: every test builds its own scene and asserts no shared state |

## 8. What this hook does not do

- It does not create, mount or position the dialog. It confines focus in a subtree the component
  already built.
- It does not set `aria-modal`, `role="dialog"` or an accessible name (section 5).
- It does not handle Escape (rule 11).
- It does not scroll the target into view; `FocusManager::request_focus` already reveals the node it
  focuses.
