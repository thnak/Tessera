# `useListFocus` v1

Status: draft, normative once accepted. Specifies the `useListFocus` behavior hook of
[ADR-064](../../decisions/ADR-064-astryx-style-component-system.md) item 7 (issue #625). The
behavior-hook machinery it stands on is [`behavior-hooks-v1.md`](behavior-hooks-v1.md); this file
adds only what is specific to this hook.

Terms: the **target** is the element the hook's `ref` names; the **group** is the engine's roving-focus
state (`Node::set_focus_group` plus `FocusManager::move_in_focus_group`); an **item** is a focusable
descendant of the target; the **Tab stop** is the one item the group exposes to sequential navigation.

## 1. Call form

```tsx
const listRef = useRef<View>(null);
const focus = useListFocus(listRef, { orientation: "vertical", typeahead: true });
return <View ref={listRef} role="listbox">{items}</View>;
```

One `const` binding at the top level of the component body, under the placement and order rules of
every hook. The target is required; the options object is optional because every option has a
default. The hook is **exclusive**: two `useListFocus` calls on one target are
`TESSERA_BEHAVIOR_DUPLICATE`, because two groups on one subtree have no defined precedence.

## 2. Arguments

| Argument | Type | Default | Meaning |
|---|---|---|---|
| `target` | a `useRef` of any element | required | the subtree whose focusable descendants form the group |
| `orientation` | `"horizontal" \| "vertical" \| "both"` | `"vertical"` | which arrow keys move focus |
| `wrap` | `boolean` | `true` | whether the arrows wrap at both ends |
| `typeahead` | `boolean` | `false` | whether typing a character moves focus to a matching item |

`orientation` and `typeahead` are reactive: changing either on a later render updates the group
without re-creating the behavior.

## 3. Outputs

| Output | Type | Meaning |
|---|---|---|
| `activeIndex` | `number` | the index of the item that holds the Tab stop, or `-1` when none does |

The index counts the target's focusable descendants in tree order, skipping invisible subtrees. That
is the same order the engine walks, so an index this hook reports and a move the engine makes agree.

## 4. Rules

Each rule is normative and has a test in `tests/unit/roving_focus_tests.cpp`.

1. **Attach marks the group.** When the behavior attaches, the target's group axis is set from
   `orientation`.
2. **The axis is reactive.** An update that changes `orientation` changes the group's axis in the same
   call, with no re-attach.
3. **The axis is respected.** A `horizontal` group ignores ArrowUp/ArrowDown and honours
   ArrowLeft/ArrowRight; a `vertical` group does the reverse; `both` honours all four.
4. **The arrows move and wrap.** ArrowDown/ArrowRight move to the next item and ArrowUp/ArrowLeft to
   the previous one, wrapping at both ends instead of leaving the group.
5. **Home and End jump.** Home moves to the first item and End to the last, whatever the axis.
6. **`typeahead` is a flag on the group.** When true, typing a character moves focus to the next item
   whose `aria-label` starts with it (`FocusManager::typeahead`); when false, typing does nothing.
7. **`activeIndex` tracks the Tab stop.** The output is the index of the focused item, or `-1` when
   focus is outside the group or nowhere.
8. **Detach clears what it set.** Detaching clears the group axis and the `typeahead` flag, so no group
   outlives its component.
9. **An unresolved target attaches nothing.** When the target ref holds no node, the behavior attaches
   nothing and marks no node.
10. **No hidden global state.** The behavior holds one target pointer and one applied flag. It
    registers no listener and reads no clock, so a headless test is deterministic.

## 5. Accessibility

- A roving group is the WAI-ARIA composite-widget pattern: the group has one Tab stop and the arrows
  move within it. Rule 4 is what makes the group reachable with one Tab press and navigable without
  leaving it.
- The component must give the target a role that matches the group (`listbox`, `tablist`, `menu`,
  `radiogroup`) and each item its own role. The hook does not set either: the role decides which
  arrow keys a screen reader announces, and guessing it would be worse than requiring it.
- Rule 7 is what a component needs to render `aria-activedescendant` or a highlight without keeping a
  second copy of the focus state.
- `wrap` is a real choice, not a default to ignore: a menu that wraps is harder to leave with the
  keyboard, so a component that wants a bounded list sets `wrap: false`.

## 6. States

| State | Group axis | `activeIndex` |
|---|---|---|
| detached | unchanged | `0` (the manifest default) |
| attached, no focus in the group | set from `orientation` | `-1` |
| attached, focus on item *n* | set from `orientation` | *n* |

## 7. Verification map

| Rule | Test |
|---|---|
| 1 | `test_attach_sets_the_group` |
| 2 | `test_orientation_selects_the_axis` |
| 3 | `test_axis_is_respected` |
| 4 | `test_arrow_keys_move_and_wrap` |
| 5 | `test_home_and_end` |
| 6 | `test_typeahead_flag` |
| 7 | `test_active_index_tracks_focus` |
| 8 | `test_detach_clears_the_group` |
| 9 | `test_missing_target_attaches_nothing` |
| 10 | the whole file: every test builds its own scene and asserts no shared state |

## 8. What this hook does not do

- It does not set roles or `aria-activedescendant` (section 5).
- It does not decide which items are focusable; the component sets `tabIndex` on its items.
- It does not handle virtualized lists differently: a virtualized item that is not realized has no
  node, so it is not an item of the group until it is. `activeIndex` therefore counts realized items,
  which is what a component rendering a window of rows needs.
- It does not implement `useTypeahead` as a separate hook: `typeahead` is a flag on this group,
  because the engine's typeahead already reads the group it belongs to.
