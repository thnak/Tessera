# `useHotkeys` v1

Status: draft, normative once accepted. Specifies the `useHotkeys` behavior hook of
[ADR-064](../../decisions/ADR-064-astryx-style-component-system.md) item 7 (issue #627). The
behavior-hook machinery it stands on is [`behavior-hooks-v1.md`](behavior-hooks-v1.md); this file
adds only what is specific to this hook.

Terms: a **chord** is a modifier set plus one key (`ctrl+s`); the **registry** is the window's
`KeyBindingRegistry` (`InputRouter::accelerators()`); a **scope** is the window, because a shortcut
is not attached to an element.

## 1. Call form

```tsx
const keys = useHotkeys({ chords: "ctrl+s,ctrl+shift+s", repeat: false });
return <View>{...}</View>;
```

One `const` binding at the top level of the component body, under the placement and order rules of
every hook. The hook takes **no target ref**: a shortcut belongs to the window, not to an element, so
there is nothing to attach it to. The options object is required because `chords` has no default.

## 2. Arguments

| Argument | Type | Default | Meaning |
|---|---|---|---|
| `chords` | `string` | required | a comma-separated chord list |
| `allowInTextInput` | `boolean` | `false` | whether a chord fires while a text field has focus |
| `repeat` | `boolean` | `false` | whether auto-repeat fires the chord more than once |

A chord is `[modifier+]*key`. A modifier is `ctrl` (or `control`), `alt`, `shift`, `meta` (or `cmd`,
`super`). A key is a single letter or digit, or a name from `key_code.hpp` without its `k` prefix
(`escape`, `enter`, `tab`, `space`, `home`, `end`, `pageup`, `pagedown`, `insert`, `delete`,
`backspace`, `arrowleft`, `arrowup`, `arrowright`, `arrowdown`, `f1`..`f12`). Matching is
case-insensitive.

## 3. Outputs

| Output | Type | Meaning |
|---|---|---|
| `registered` | `boolean` | whether at least one chord is currently registered |

## 4. Rules

Each rule is normative and has a test in `tests/unit/hotkeys_tests.cpp`.

1. **A chord parses to a key and a modifier set.** `ctrl+s` is key `S` with `Control`; `shift+alt+f1`
   is `F1` with both; `escape` is `Escape` with none.
2. **An unknown modifier, key or empty segment is rejected.** `hyper+s`, `ctrl+`, `ctrl+nosuchkey`
   and the empty string do not parse.
3. **Attach registers every chord.** Each parsed chord becomes one registry binding, and the
   `registered` output is true when at least one was accepted.
4. **A registered chord fires through the router.** Dispatching a `KeyDown` with the chord's key and
   modifiers reports `accelerator_fired`.
5. **A different modifier set does not fire.** The registry's exact-match rule is the contract: a
   `ctrl+s` binding does not fire for `ctrl+shift+s`.
6. **A malformed chord is skipped, not fatal.** The other chords in the list still register.
7. **Detach removes exactly what was registered.** The registry returns to its previous size and the
   chord no longer fires.
8. **An update replaces the set.** Re-registering removes the old bindings first, so a component that
   re-renders does not accumulate them.
9. **A build with no router registers nothing.** The behavior reports `registered = false` rather
   than crashing.
10. **A destroyed behavior removes its bindings.** A behavior destroyed without an explicit detach
    removes them in its destructor, so the window cannot keep firing a shortcut for a component that
    no longer exists.
11. **The behavior never invokes a callback.** It registers a marker binding; the input path reports
    the fired id through `InputDispatchResult::accelerator_fired`, and the generated base turns that
    into the component's handler call. A behavior may not invoke a callback
    (behavior-hooks-v1.md section 8).
12. **A text field suppresses a chord unless `allowInTextInput` is set.** While the focused node is a
    `TextInput` or `TextArea`, the behavior registers nothing and reports `registered = false`; with
    the option set it registers normally.

## 5. Accessibility

- A shortcut is an **accelerator**, never the only way to reach a command. WCAG 2.1.1 (Keyboard)
  requires every function to be reachable from the keyboard through the normal focus order; a
  component that offers only a chord fails it.
- Rule 5 is what makes a chord predictable: a user who presses `ctrl+shift+s` expecting "save as"
  must not trigger "save".
- `allowInTextInput` defaults to false because a chord that fires while a text field has focus steals
  a key the user is typing. A component that wants `ctrl+s` to work while editing opts in explicitly.
- A chord is listed for assistive technology through the component's own hint text; the hook does not
  announce it, because a shortcut with no visible affordance is not discoverable by a screen reader
  either.
## 6. States

| State | Registry | `registered` |
|---|---|---|
| detached | unchanged | `false` |
| attached, no valid chord | unchanged | `false` |
| attached, at least one chord | one binding per chord | `true` |
| updated | the new set only | as the new set |

## 7. Verification map

| Rule | Test |
|---|---|
| 1 | `test_parse_chord` |
| 2 | `test_parse_chord` |
| 3 | `test_attach_registers` |
| 4 | `test_chord_fires` |
| 5 | `test_unregistered_chord_does_not_fire` |
| 6 | `test_malformed_chord_is_skipped` |
| 7 | `test_detach_removes` |
| 8 | `test_update_replaces` |
| 9 | `test_no_router_registers_nothing` |
| 10 | `test_destructor_removes` |
| 11 | `test_chord_fires` (the binding is a marker; the router reports the id) |
| 12 | `test_text_input_suppresses_by_default` |

## 8. What this hook does not do

- It does not run the handler: the generated base does, from the fired id (rule 11).
- It does not implement scopes beyond the window. "Most specific scope wins" needs a scope stack the
  engine does not have; a component that needs it registers a chord and checks its own state.
- It does not report conflicts between two components' chords. The registry's rule is "first
  registration wins", which is deterministic; a dev-mode conflict report is a follow-up.
- It does not map `Cmd` to `Ctrl` per platform profile (ADR-034). `meta` is `meta`; a component that
  wants the platform's primary modifier reads the profile itself.
