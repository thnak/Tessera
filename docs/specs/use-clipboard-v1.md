# `useClipboard` v1

Status: draft, normative once accepted. Specifies the `useClipboard` behavior hook of
[ADR-064](../../decisions/ADR-064-astryx-style-component-system.md) item 7 (issue #632). The
behavior-hook machinery it stands on is [`behavior-hooks-v1.md`](behavior-hooks-v1.md); this file
adds only what is specific to this hook.

Terms: the **seam** is `hal::Clipboard` (`include/tessera/hal/clipboard.hpp`); the **feedback
window** is how long `copied`/`failed` stay true after a copy.

## 1. Call form

```tsx
const copy = useClipboard({ text: code });
return <View role="button">{copy.copied ? "Copied" : "Copy"}</View>;
```

One `const` binding at the top level of the component body, under the placement and order rules of
every hook. The hook takes **no target ref**: the clipboard belongs to the window, not to an element.
The options object is required because `text` has no default.

## 2. Arguments

| Argument | Type | Default | Meaning |
|---|---|---|---|
| `text` | `string` | required | the text to copy |

## 3. Outputs

| Output | Type | Meaning |
|---|---|---|
| `copied` | `boolean` | the last copy succeeded, within the feedback window |
| `failed` | `boolean` | the last copy failed, within the feedback window |

## 4. Rules

Each rule is normative and has a test in `tests/unit/clipboard_copy_tests.cpp`.

1. **A copy writes through the seam.** Attaching with non-empty `text` calls
   `Clipboard::write_text`, and the seam holds the text afterwards.
2. **Success reports `copied`.** A write the platform accepted sets `copied` true and `failed` false.
3. **Failure reports `failed`.** A write the platform refused (`Unsupported`, `Busy`, `TooLarge`,
   `Failed`) sets `failed` true and `copied` false.
4. **No seam is a failure, not a crash.** A build with no clipboard reports `failed` true.
5. **Empty text copies nothing.** An empty `text` does not overwrite the user's clipboard and
   reports no copy: silently replacing the clipboard with an empty string is worse than doing
   nothing.
6. **The feedback expires.** `copied` and `failed` clear after `kFeedbackSeconds` (2 s), measured
   from the copy, so a "Copied" label does not stay on screen forever.
7. **The expiry is a deadline, not a timer.** The behavior reads the time from `Services::now_s` and
   compares it in `tick`, so a `ManualClock` test advances it explicitly and no thread or timer
   exists.
8. **A failure's feedback expires too.** A "Copy failed" label clears on the same deadline.
9. **Detach resets.** Detaching clears both outputs and releases the seam.
10. **A destroyed behavior releases the seam.** A behavior destroyed without an explicit detach
    releases it in its destructor.
11. **The UI thread never blocks.** The seam's contract is that both directions are bounded and
    return a value; the behavior does not wait, retry or spin.

## 5. Accessibility

- The feedback is **visual only**. A component that shows "Copied" must also announce it
  (`useAnnounce`, issue #629) or a screen-reader user gets no confirmation that the copy happened.
- Rule 5 is a data-safety rule, not a nicety: a copy button whose text is empty (a code block that
  failed to load) must not wipe the clipboard the user was carrying.
- The component must give the copy control a role and a name (`role="button"`, an accessible label);
  the hook sets neither.

## 6. States

| State | `copied` | `failed` |
|---|---|---|
| detached | `false` | `false` |
| copied, inside the window | `true` | `false` |
| failed, inside the window | `false` | `true` |
| window expired | `false` | `false` |
| empty text | `false` | `false` |

## 7. Verification map

| Rule | Test |
|---|---|
| 1 | `test_copy_writes_and_reports` |
| 2 | `test_copy_writes_and_reports` |
| 3 | `test_unsupported_clipboard_reports_failure` |
| 4 | `test_no_clipboard_reports_failure` |
| 5 | `test_empty_text_copies_nothing` |
| 6 | `test_feedback_clears_after_the_window` |
| 7 | `test_feedback_clears_after_the_window` (the test advances the clock explicitly) |
| 8 | `test_failure_feedback_also_expires` |
| 9 | `test_detach_resets` |
| 10 | `test_destructor_resets` |
| 11 | evidence gap: the seam's own tests (`clipboard`) cover the bounded, non-blocking contract; this hook has no wait path to test |

## 8. What this hook does not do

- It does not read the clipboard; a paste is the text-editing runtime's job
  (`docs/specs/text-editing-v1.md` section 12).
- It does not announce the copy (section 5).
- It does not copy rich text, HTML or images: the seam is UTF-8 text only.
- It does not retry a `Busy` clipboard: the seam already bounds its own retry, and a hook that
  retried would block the UI thread.
