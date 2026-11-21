# `usePressFeedback` v1

Status: draft, normative once accepted. Specifies the `usePressFeedback` behavior hook of
[ADR-064](../../decisions/ADR-064-astryx-style-component-system.md) item 7 (issue #628). The
behavior-hook machinery it stands on is [`behavior-hooks-v1.md`](behavior-hooks-v1.md); this file
adds only what is specific to this hook.

Terms: the **target** is the element the hook's `ref` names; **pressed** is the scene's active
pointer or keyboard activation on the target; **hovered** is the pointer resting on it; **focused** is
keyboard focus on it.

## 1. Call form

```tsx
const buttonRef = useRef<View>(null);
const press = usePressFeedback(buttonRef, { disabled: props.disabled });
return <View ref={buttonRef} role="button" aria-pressed={press.pressed}>{...}</View>;
```

One `const` binding at the top level of the component body, under the placement and order rules of
every hook. The target is required; the options object is optional because every option has a
default. The hook is not exclusive: a component may attach it to several targets.

## 2. Arguments

| Argument | Type | Default | Meaning |
|---|---|---|---|
| `target` | a `useRef` of any element | required | the element whose interaction state is reported |
| `disabled` | `boolean` | `false` | when true, the target never reports pressed |

## 3. Outputs

| Output | Type | Meaning |
|---|---|---|
| `pressed` | `boolean` | the target is being pressed |
| `hovered` | `boolean` | the pointer is over the target |
| `focused` | `boolean` | the target holds keyboard focus |

The three are independent: a pointer press on a hovered button reports both, and a keyboard
activation reports `pressed` without `hovered`.

## 4. Rules

Each rule is normative and has a test in `tests/unit/press_feedback_tests.cpp`.

1. **Defaults before attach.** Before the first attach and after detach, all three outputs are
   `false`.
2. **A pointer press reports pressed and hovered.** A pointer-down on the target sets `pressed` and
   `hovered`; a pointer-up clears `pressed`.
3. **Leaving clears hovered.** Moving the pointer off the target clears `hovered` and, if the press
   was still down, `pressed` too: a press that moves off the target is cancelled.
4. **Focus is independent.** Keyboard focus on the target sets `focused` without setting `pressed` or
   `hovered`; a keyboard activation sets `pressed` without `hovered`.
5. **`disabled` suppresses pressed.** When `disabled` is true the target never reports `pressed`,
   whatever the pointer or keyboard does. `hovered` and `focused` still report, because a disabled
   control that is focusable must still show where focus is (ADR-064 item 9).
6. **Detach resets and is idempotent.** Detaching clears every output to `false`; a second detach is a
   no-op.
7. **An unresolved target attaches nothing.** When the target ref holds no node, the behavior attaches
   nothing and reports the defaults.
8. **A dispatch with no scene is a no-op.** A build with no `InputRouter` reports the defaults rather
   than crashing.
9. **An update with no options keeps the state.** The hook has no reactive option that changes the
   outputs, so an update recomputes the same values rather than resetting them.
10. **No hidden global state.** The behavior holds one target pointer and one router pointer. It
    registers no listener and reads no clock, so a headless test is deterministic.

## 5. Accessibility

- The outputs are for **styling**, not for semantics: a component sets `aria-pressed` from its own
  state, not from `press.pressed`, because `aria-pressed` is a toggle state and `press.pressed` is a
  transient interaction. A component that conflated them would announce a toggle that flips on every
  click.
- Rule 5 is the WCAG 3.2.1 (On Focus) and 4.1.2 (Name, Role, Value) requirement: a disabled control
  must still be focusable and must still show focus, or a keyboard user cannot tell what is disabled
  and why.
- The hook reports state; it does not set `:hover`/`:active` styles. A component styles from the
  outputs or from the stylesheet's pseudo-classes, never from inline styles (ADR-064 item 5).

## 6. States

| State | `pressed` | `hovered` | `focused` |
|---|---|---|---|
| detached | `false` | `false` | `false` |
| idle | `false` | `false` | `false` |
| hovered | `false` | `true` | `false` |
| pressed | `true` | `true` | `false` |
| focused | `false` | `false` | `true` |
| disabled | `false` | as the pointer | as focus |

## 7. Verification map

| Rule | Test |
|---|---|
| 1 | `test_defaults_before_attach` |
| 2 | `test_pointer_press_reports_pressed_and_hovered` |
| 3 | `test_pointer_leave_clears_hovered` |
| 4 | `test_keyboard_focus_is_independent` |
| 5 | `test_disabled_suppresses_pressed_only` |
| 6 | `test_detach_is_idempotent_and_resets` |
| 7 | `test_unattached_target_is_a_no_op` |
| 8 | `test_null_dispatch_is_a_no_op` |
| 9 | `test_update_with_no_options_keeps_the_state` |
| 10 | the whole file: every test builds its own scene and asserts no shared state |

## 8. What this hook does not do

- It does not set `aria-pressed` (section 5).
- It does not style anything; the outputs are values a component reads.
- It does not implement long-press or double-click; those are separate behaviors
  (ADR-064 item 7, "after v1").
