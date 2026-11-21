# `useAnnounce` v1

Status: draft, normative once accepted. Specifies the `useAnnounce` behavior hook of
[ADR-064](../../decisions/ADR-064-astryx-style-component-system.md) item 7 (issue #629). The
behavior-hook machinery it stands on is [`behavior-hooks-v1.md`](behavior-hooks-v1.md); this file
adds only what is specific to this hook.

Terms: the **queue** is `accessibility::AnnouncementQueue`, the bounded list the accessibility bridge
drains; a **channel** is `aria-live`'s `polite`/`assertive`/`off`; an **announcement** is one queued
message.

## 1. Call form

```tsx
const saved = useAnnounce({ message: "Saved", channel: "polite" });
return <View>{saved.announced ? "Saved" : "Save"}</View>;
```

One `const` binding at the top level of the component body, under the placement and order rules of
every hook. The hook takes **no target ref**: an announcement has no element, because it is a message
rather than content. The options object is required because `message` has no default.

## 2. Arguments

| Argument | Type | Default | Meaning |
|---|---|---|---|
| `message` | `string` | required | the text to announce |
| `channel` | `"polite" \| "assertive" \| "off"` | `"polite"` | the `aria-live` channel |

## 3. Outputs

| Output | Type | Meaning |
|---|---|---|
| `announced` | `boolean` | whether this call queued a new announcement |

## 4. Rules

Each rule is normative and has a test in `tests/unit/announce_tests.cpp` (the behavior) or
`tests/unit/announcement_queue_tests.cpp` (the queue).

1. **Attach queues the message.** A non-empty `message` on a live channel becomes one queue entry.
2. **The channel is carried.** `polite` and `assertive` reach the queue as themselves; `off` queues
   nothing.
3. **A repeated message is not a new announcement.** The same text on the same channel as the last
   announcement is suppressed, both by the behavior and by the queue, so a component that re-renders
   on every keystroke does not make a screen reader repeat itself.
4. **A different message is a new announcement.** Changing `message` queues it.
5. **An empty message announces nothing.** A component whose message is empty (a form with no error
   yet) queues nothing rather than announcing silence.
6. **No queue is not a crash.** A build with no accessibility bridge reports `announced = false`.
7. **Assertive interrupts.** An assertive announcement is delivered before any queued polite one.
8. **The queue is bounded.** At most `kMaxQueued` (16) announcements are held; the oldest polite one
   is dropped first, and an assertive one is never dropped.
9. **The text is bounded.** A message longer than `kMaxTextBytes` (512) is truncated rather than
   rejected: losing the tail of a message is better than losing the message.
10. **A scope can be cleared.** A modal layer that closes drops the announcements it scoped, so a
    message about a dialog the user just dismissed is not read afterwards.
11. **An announcement outlives its component.** Detaching the behavior does not remove the queued
    message: a toast that announces "Saved" and then unmounts still has to be announced.
12. **The queue is the window's, not a component's.** The behavior writes into the window's queue
    through `Services`; it does not own one.

## 5. Accessibility

- This hook is the **only** way a component tells a screen-reader user about a change that is not a
  focus move: a save, a validation error, a pending state. A component that shows a visual toast
  without announcing it leaves a screen-reader user with no feedback at all.
- `polite` is the default because it waits for the user to be idle. `assertive` interrupts, so it is
  reserved for errors and time-critical state; a component that announces everything assertively
  makes the application unusable with a screen reader.
- Rule 3 is what makes the hook safe to call from a render: a component that announces on every
  render would otherwise flood the queue.
- The hook does not set `aria-live` on a node. A live region on a node is the declarative form and
  belongs to the component's markup; this hook is the imperative form for a message that has no
  element.

## 6. States

| State | Queue | `announced` |
|---|---|---|
| detached | unchanged | `false` |
| attached, empty message | unchanged | `false` |
| attached, `off` channel | unchanged | `false` |
| attached, new message | one entry added | `true` |
| attached, repeated message | unchanged | `false` |
| no queue | unchanged | `false` |

## 7. Verification map

| Rule | Test |
|---|---|
| 1 | `test_attach_queues_the_message` |
| 2 | `test_assertive_channel`, `test_off_channel_queues_nothing` |
| 3 | `test_repeated_message_is_not_announced`, `test_duplicate_is_suppressed` |
| 4 | `test_repeated_message_is_not_announced` |
| 5 | `test_empty_message_announces_nothing` |
| 6 | `test_no_queue_announces_nothing` |
| 7 | `test_assertive_goes_first` |
| 8 | `test_queue_is_bounded` |
| 9 | `test_text_is_truncated` |
| 10 | `test_scope_clear` |
| 11 | `test_detach_resets` |
| 12 | `test_no_queue_announces_nothing` (the behavior holds no queue of its own) |

## 8. What this hook does not do

- It does not deliver the announcement to the platform. The accessibility bridge drains the queue and
  maps it to UIA/AT-SPI/NSAccessibility; that mapping is the bridge's contract, not this hook's.
- It does not set `aria-live` on a node (section 5).
- It does not wait for the announcement to be read. A component that needs to know when the user
  heard it has no such signal, and inventing one would be a lie.
- It does not translate the message. A component that needs a localized string passes one.
