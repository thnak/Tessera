# `useInteractiveRole` v1

Status: draft, normative once accepted. Specifies the `useInteractiveRole` behavior hook of
[ADR-064](../../decisions/ADR-064-astryx-style-component-system.md) item 7 (issue #635). The
behavior-hook machinery it stands on is [`behavior-hooks-v1.md`](behavior-hooks-v1.md); this file
adds only what is specific to this hook.

Terms: the **target** is the element the hook's `ref` names; an **interactive surface** is a node
whose whole area is a control (a clickable card, a selectable row); a **nested** node is one with an
interactive surface among its ancestors.

## 1. Call form

```tsx
const cardRef = useRef<View>(null);
const card = useInteractiveRole(cardRef, { interactive: true });
return <View ref={cardRef} role="button">{...}</View>;
```

One `const` binding at the top level of the component body, under the placement and order rules of
every hook. The target is required; the options object is optional because the one option has a
default. The hook is **exclusive** per target: two calls on one element are
`TESSERA_BEHAVIOR_DUPLICATE`, because the flag is a property of the element, not a count.

## 2. Arguments

| Argument | Type | Default | Meaning |
|---|---|---|---|
| `target` | a `useRef` of any element | required | the element that is the interactive surface |
| `interactive` | `boolean` | `true` | whether the surface is interactive |

## 3. Outputs

| Output | Type | Meaning |
|---|---|---|
| `nested` | `boolean` | whether this surface is itself inside another interactive surface |

## 4. Rules

Each rule is normative and has a test in `tests/unit/interactive_role_tests.cpp`.

1. **Attach marks the target.** When the behavior attaches with `interactive` true, the target's
   interactive flag is set.
2. **A descendant sees the flag.** `InteractiveRole::has_interactive_ancestor(node)` is true for
   every descendant of the target, and false for a node outside it.
3. **The surface does not count itself.** A node is not its own ancestor, so the target's own
   `has_interactive_ancestor` is false unless it is nested in another surface.
4. **`nested` reports the surface's own nesting.** The output is true when the target is itself
   inside another interactive surface, which is what a component needs to decide whether to render
   its own control.
5. **`interactive` is reactive.** An update that clears the option clears the flag and its
   descendants stop seeing it, with no re-attach.
6. **Detach clears what it set.** Detaching clears the flag, so no surface outlives its component.
7. **An unresolved target attaches nothing.** When the target ref holds no node, the behavior
   attaches nothing and marks no node.
8. **The flag lives on the node, not in the behavior.** A descendant is a different component,
   possibly in a different module; the scene is the only thing they share, so the flag is on the node
   and a descendant reads it by walking its ancestors.
9. **No hidden global state.** The behavior holds one target pointer and one applied flag. It
   registers no listener and reads no clock.

## 5. Accessibility

- This hook exists for one WCAG defect: **a control inside a control**. A screen reader announces
  both, and a pointer press on the inner one also triggers the outer one because the click bubbles.
  Rule 2 is what lets the inner component render a plain element (or move itself out of the clickable
  region) instead of nesting a second control.
- The hook does not set `role` or `tabIndex`. A component that makes a card interactive must give it
  a role and a name, and must make its own keyboard activation work; the hook only answers "am I
  nested".
- Rule 4 is what a nested surface needs: a button inside a clickable card is itself a control, and
  the component that renders it must know it is nested rather than assume it is not.
- A component that hides its inner control when nested must still expose the action another way (a
  menu, a row action), or the user loses it.

## 6. States

| State | Target flag | `nested` |
|---|---|---|
| detached | unchanged | `false` |
| inactive | clear | `false` |
| active, top-level surface | set | `false` |
| active, surface inside another | set | `true` |

## 7. Verification map

| Rule | Test |
|---|---|
| 1 | `test_attach_marks_the_target` |
| 2 | `test_descendant_sees_the_parent` |
| 3 | `test_descendant_sees_the_parent` |
| 4 | `test_nested_surface_reports_nested` |
| 5 | `test_update_toggles` |
| 6 | `test_detach_clears` |
| 7 | `test_missing_target_attaches_nothing` |
| 8 | `test_descendant_sees_the_parent` (the descendant is a different node, not the behavior's owner) |
| 9 | the whole file: every test builds its own scene and asserts no shared state |

## 8. What this hook does not do

- It does not set `role`, `tabIndex` or an accessible name (section 5).
- It does not stop a click from bubbling. A component that renders a nested control must stop
  propagation itself, or the outer handler still runs; the hook tells it the situation, it does not
  change the event path.
- It does not diagnose a nesting at build time. A dev-mode diagnostic ("a button inside a button") is
  a follow-up; the hook reports the fact at runtime, which is what a component needs to avoid the
  nesting in the first place.
- It does not cover a nesting that crosses a component boundary the scene does not model (a portal or
  an overlay): the ancestor walk follows the scene tree, so a control in a separate overlay layer is
  not nested.
