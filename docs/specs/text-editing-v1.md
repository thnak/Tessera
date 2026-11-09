# Text editing v1

Status: proposed (Big Steps B2 and C, defined in
[`AGENTS.md`](../../AGENTS.md#big-steps-group-work-dont-micro-step)). Decision:
[ADR-029](../../decisions/ADR-029-interaction-completeness.md) (text editing, scrolling/gestures,
overlays, forms). Companion to
[`text.md`](../design/text.md), [`input.md`](../design/input.md),
[`forms-and-validation-v1.md`](forms-and-validation-v1.md), and
[`accessibility-v1.md`](accessibility-v1.md).

Terms: **V1** is the first release scope ([`AGENTS.md`](../../AGENTS.md#non-negotiables), item 8).
**IME** is the platform input method editor (composition for CJK and similar). **Grapheme
cluster** is one user-perceived character. **Bidi** is mixed left-to-right/right-to-left text.
**HAL** is the hardware/OS abstraction layer. **Golden** is a deterministic reference image. See
the [glossary](../reference/glossary.md). Gap ids (`G-01` to `G-05`) refer to rows in
[`app-completeness-gap-resolutions-v1.md`](../architecture/app-completeness-gap-resolutions-v1.md).

Text editing is a core primitive, not an app-level widget. It must work with shaping, bidi,
IME, accessibility, clipboard, selection, undo/redo, and deterministic tests.

Input plumbing status: the normalized input model and scene routing for text/IME are implemented
(`InputEventType::CompositionStart/Update/End` and `EditText`, bounded inline UTF-8 payload with
truncation, UTF-16 code-unit offsets, focused-node-only delivery, Win32 `WM_IME_*` translation;
see [`input.md`](../design/input.md) sections 2.1 and 8). The editing core is implemented in
`include/tessera/ui/text/edit_buffer.hpp` and `edit_controller.hpp` (UTF-8 buffer, caret/selection,
grapheme/word/line commands, bounded length, IME composition commit/cancel; see section 12).
Editable-node scene integration, undo/redo, clipboard, bidi visual movement, candidate-window
positioning, and IME producers beyond Win32 remain future work. The editable field path is
demonstrated headlessly in `tests/integration/generated_ui_tests.cpp`: the edit controller's caret
and half-open selection are published as `PaintData::decoration` (UTF-8 byte offsets) and painted
as caret/selection pixels; the `editing.png` CPU golden covers it. Committed-edit change events and
the typed handler adapter are implemented in `tessera_text_edit`
(`EventType::TextChange`, `TextInputRuntime`, `TextChangeBinding`); the controlled `value`/`onChange`
contract codegen must emit is recorded in [`compiler.md`](../design/compiler.md) section 11.

## 1. Scope

- Single-line `TextInput`, multi-line `TextArea`.
- Caret, selection, composition, editing commands, undo/redo, clipboard.
- Keyboard/mouse/touch/IME/autofill integration.
- Accessibility and headless tests.
- Rich text is out of scope for V1; text/plain is the baseline.

## 2. Model

```cpp
using TextOffset = std::uint32_t;   // logical grapheme index

struct TextSelection {
    TextOffset anchor = 0;
    TextOffset focus = 0;           // caret position
    TextAffinity affinity = TextAffinity::Downstream;
};

struct TextEditState {
    std::string text;               // UTF-8 storage
    TextSelection selection;
    TextSelection composition;      // optional marked range
    bool composing = false;
    bool read_only = false;
};
```

Rules:

- Store UTF-8; expose grapheme-cluster offsets, never raw bytes to UI code.
- Grapheme/word/line boundaries come from ICU/HarfBuzz; bidi visual positions come from the
  text layout.
- Logical offsets are stable across bidi; visual caret rectangles are layout-derived.
- Selection is anchor/focus; a collapsed selection is a caret.
- Composition is a marked range with a temporary replacement string; commit replaces atomically.
- `TextEditState` is per field; no global editing state.

## 3. Commands

Commands are explicit values (no arbitrary JS):

```text
insert_text(text)
replace_selection(text)
delete_backward/forward(unit: grapheme|word|line)
move_caret(direction, unit, extend)
select_all / select_word / select_line / select_range
copy / cut / paste
undo / redo
set_composition(text, range) / commit_composition / cancel_composition
scroll_into_view(caret)
```

- Each command returns a result and a change descriptor; undo/redo is built from changes.
- Typing coalesces into one undo group; paste/IME commit is one group; selection-only changes
  do not create undo entries.
- Key bindings are platform-aware but commands are portable; shortcuts are declared by the
  app/component.
- Commands never allocate unbounded history; the history is capped by bytes/entries.

## 4. IME and text input

- The platform/HAL owns the native IME and candidate-window placement; Tessera normalizes
  composition start/update/end, committed text, and dead keys, and reports the caret rect to the
  platform (see [`input.md`](../design/input.md)).
- Composition text is rendered with underline/marked style; caret stays at the composition
  boundary; committed text replaces the marked range atomically.
- Candidate window is positioned near the caret, flipped/clamped to the window and safe area.
- Dead keys and key repeat are delivered by the platform; editing commands see the final text.
- Secure fields disable clipboard/history/autofill by default and redact logs/snapshots.
- Input modes (`text`, `numeric`, `email`, `url`, `search`, `tel`) are declared props; the
  platform IME/keyboard uses them.
- No blocking in composition handlers; no layout/render in the platform callback.

## 5. Undo/redo

- Per-field command stack; max entries/bytes configurable.
- Typing coalescing grouped by time/word boundary; composition and paste are atomic.
- Undo/redo restores text, selection, and scroll-into-view; it does not replay validation or
  side effects.
- Redo is cleared by a new edit; external value changes reset history (no further rule is specified).
- History is never persisted by default; state restoration may opt in for drafts (see
  [`state-restoration-v1.md`](state-restoration-v1.md)).

## 6. Clipboard

- V1: text/plain only. HTML/RTF/file lists are not supported in V1 (HTML/RTF pasted content is
  sanitized to plain text, see G-05); no release for them is specified.
- Cut/copy/paste use the platform shell (see [`platform-shell-v1.md`](platform-shell-v1.md));
  clipboard access is async and permission-aware.
- Paste normalizes line endings and rejects oversized content; no unbounded allocation.
- Password/secure fields: copy/cut disabled by default; paste allowed but never logged.
- Selection drag/copy is a pointer gesture; no implicit clipboard access without a command.

## 7. Nodes and JSX

```tsx
<TextInput
  value={email}
  onChange={setEmail}
  placeholder="Email"
  inputMode="email"
  autofill="email"
  maxLength={254}
  onSelectionChange={handleSelection}
  onSubmit={submit}
/>

<TextArea
  value={notes}
  onChange={setNotes}
  rows={6}
  wrap="word"
  readOnly={locked}
/>
```

Node kinds: `TextInput`, `TextArea` (leaf intrinsics). Props:

| Prop | Type | Notes |
|---|---|---|
| `value` | string | Controlled value; source of truth in app state |
| `defaultValue` | string | Uncontrolled initial value |
| `placeholder` | string | Not a value; never submitted |
| `readOnly` / `disabled` | bool | Edit/focus behavior |
| `secure` | bool | Password semantics/redaction |
| `maxLength` | int | Grapheme limit; no silent truncation of composition |
| `inputMode` | enum | Keyboard/IME hint |
| `autofill` | enum | Platform autofill hint |
| `rows` / `wrap` | int/enum | Multiline layout |
| `selection` | TextSelection | Controlled selection, optional |
| `scrollPolicy` | enum | `caret`, `none`, `always` |

Events: `onChange`, `onSubmit`, `onFocus`, `onBlur`, `onSelectionChange`,
`onCompositionStart/Update/End`, `onEditCommand` (advanced).

- Controlled mode: the field applies edits optimistically, then syncs `value`; a rejected value
  restores the previous state deterministically.
- Uncontrolled mode owns state internally and exposes it via ref/snapshot.
- `TextArea` scrolls the caret into view; `TextInput` never wraps.
- Validation/errors are form concerns, not text editing; see [`forms-and-validation-v1.md`](forms-and-validation-v1.md).

## 8. Layout, paint, and accessibility

- Caret blink uses the runtime clock; deterministic mode (see
  [`runtime-config-v1.md`](runtime-config-v1.md) section 5.5) pins it off/on for goldens.
- Selection highlight follows visual runs (bidi), not byte ranges.
- Composition underline, caret, and selection are paint primitives; no platform draw calls.
- Text scale/font fallback reflows; caret/selection hit testing uses shaped clusters.
- Scroll caret into view uses the scroll system ([`scroll-and-gestures-v1.md`](scroll-and-gestures-v1.md));
  no layout feedback loop.
- Accessibility exposes text, selection range, caret, placeholder, read-only/disabled, and
  actions (set value, select, copy/cut/paste); see [`accessibility-v1.md`](accessibility-v1.md).

## 9. Determinism and tests

- Fixed clock, locale, fonts, input script; no system IME in goldens.
- Test: insertion/deletion by grapheme/word/line; bidi caret/selection; shaping clusters;
  IME composition/commit/cancel; dead keys; undo/redo grouping; clipboard formats;
  maxLength; secure redaction; scroll-into-view; accessibility snapshots.
- Headless: synthetic key/text/composition events; snapshot text/selection/caret/history.
- Golden: caret/selection/composition at fixed clock and fonts.

## 10. Security and privacy

- Secure fields: no plaintext logs, snapshots, traces, clipboard, autofill, or undo persistence
  unless explicitly enabled.
- Bound paste/drop/text size; reject malformed UTF-8; no raw pointers in state.
- Clipboard reads/writes are platform permissions; validate file paths for file drops.
- Never execute or interpret pasted content.

## 10.1 Detail design

Ownership:

| Component | Owns | Does not own |
|---|---|---|
| `TextDocument` | UTF-8 bytes, grapheme/paragraph index, revision | Selection, focus, undo policy |
| `TextEditController` | selection, composition, undo/redo, commands | Shaping, caret geometry, native IME |
| `TextLayout` | shaped runs, caret/selection rects, bidi mapping | Text bytes/editing state |
| Input/HAL | normalized text/composition events, caret rect to platform | Editing policy, undo |
| `FormController` | value/validation/submit | Text commands/selection |

Recommended data structures:

- `TextDocument` uses a contiguous UTF-8 buffer up to 64 KiB and a piece table above it
  (G-01 below, ADR-033), plus a grapheme index. The index is versioned by `revision`.
- `TextEditController` stores `selection`, `composition`, `undo_stack`, `redo_stack`, and
  `preferred_visual_column` (for up/down movement).
- `TextLayout` caches shaped runs by `(revision, width, style, scale)`; caret/selection rects are
  derived from clusters, not bytes.

Controller state machine:

```text
Blurred -> Focused -> Editing
Focused -> Composing -> Editing (commit/cancel)
Focused -> DragSelect -> Editing
ReadOnly/Disabled: focus/select/copy only; no mutation
```

Event order per tick:

```text
platform text/composition -> input queue -> focused TextEditController
  -> document/selection/undo update -> change events
  -> layout invalidation -> paint -> accessibility snapshot
```

Rules/invariants:

- All edits go through commands; no direct document mutation from UI handlers.
- A command is atomic: text, selection, undo entry, and change descriptor commit together.
- `maxLength` counts graphemes; composition is never truncated mid-cluster. Over-limit commits
  are rejected as a whole with a diagnostic.
- External `value` changes are diffed against the current document; if composing, commit/cancel
  first. A stale controlled update is ignored by revision.
- Caret affinity is preserved across bidi runs; up/down uses visual columns, left/right uses
  logical grapheme movement.
- TextArea internal scroll is a `Scroll` container; scrolling never changes selection.
- The platform positions the native IME candidate window; the UI supplies the caret rect and
  never owns the native window.

Failure/edge cases:

- IME cancel restores the pre-composition text/selection.
- Paste over limit is rejected as a whole with a diagnostic (same rule as over-limit commits, section on `maxLength`); no truncation, so no partial cluster.
- Invalid UTF-8 is rejected at the boundary; never repair silently.
- Clipboard denied returns an error; the field remains unchanged.
- Selection outside document bounds clamps to the nearest grapheme.
- Undo during composition is ignored until commit/cancel.
- Secure fields never appear in undo persistence, snapshots, logs, or clipboard by default.

Gap-closure decisions:

- **G-01**: contiguous UTF-8 buffer up to 64 KiB; piece table above it; incremental
  grapheme/paragraph index versioned by `revision`. Undo cap 4 MiB/256 commands per field.
- **G-02**: `ui/editing` owns selection handles (44 dp); long-press selects a word; handle drag
  claims the gesture arena before scroll; platform magnifier if available, framework loupe
  fallback.
- **G-03**: autofill uses the platform credential/autofill service via the `autofill` hint; no
  framework password vault; secure fields never log/persist/clipboard by default.
- **G-04**: platform-local spellcheck/autocorrect only, disabled for secure/code fields; no
  network provider/custom dictionaries in V1.
- **G-05 deferred**: clipboard is text/plain only; HTML/RTF is sanitized to plain text; rich-text
  editing is out of scope.

## 11. Deferred/open questions

- Undo/draft persistence for drafts (opt-in via restore; not implemented in V1; the restore table in
  [`state-restoration-v1.md`](state-restoration-v1.md) section 2 excludes undo history).
- Multi-caret/column selection and text search/replace (after V1; no release specified).
- IME candidate-window styling on multi-monitor/foldable devices (platform UI).
- Touch magnifier visual parity across platforms (best-effort).

## 12. Implemented subset: the editing core

`include/tessera/ui/text/edit_buffer.hpp` + `src/ui/text/edit_buffer.cpp` (in `tessera_text`) and
`include/tessera/ui/text/edit_controller.hpp` + `src/ui/text/edit_controller.cpp` (in
`tessera_text_edit`, which also links `tessera_scene` for the normalized event types) implement the
editing primitive this layer was designed for. The reduced grapheme rule below is deliberate: no
ICU dependency in this slice.

Buffer and caret model:

- UTF-8 storage; caret and selection anchor are **byte offsets** that are always grapheme-cluster
  boundaries. `CaretPosition` reports the same position as bytes, code points, and grapheme
  clusters (counted from the buffer start, O(n) per query).
- Bounds: `max_bytes` (constructor, default `kMaxEditTextBytes = 64 KiB`, G-01 contiguous cap).
  Growth is `std::string`'s amortized growth; `reserve()` (bounded by the cap) lets a caller edit
  without allocation until the cap is reached. There is no per-keystroke allocation beyond the
  buffer's own growth.
- Commands return `EditResult { status, changed, truncated, caret, anchor }`; statuses are
  `Ok`, `NoChange`, `InvalidUtf8`, `OutOfRange` (reserved), `Truncated`. `changed` also covers
  caret/selection-only updates. No exceptions are thrown for editing errors.
- Clamping: offsets clamp to `[0, size]` and snap **down** to the containing cluster start;
  `insert` replaces the selection (collapsed selection = caret insertion) and leaves the caret
  after the inserted text; `replace_range(start, end, text)` is the explicit composition-commit
  form (offsets clamp, swapped when reversed).
- Movement: grapheme left/right (`prev_cluster`/`next_cluster`), word left/right, line
  home/end, document home/end, `select_all`; plain arrows collapse a selection to its leading edge
  (left: start, right: end) and Shift extends it while keeping the anchor.

Documented simplified grapheme cluster rule (UAX #29 reduced set):

1. One base code point, then any following **Extend** code points: combining marks in
   U+0300–U+036F, U+1AB0–U+1AFF, U+1DC0–U+1DFF, U+20D0–U+20FF, U+FE20–U+FE2F; variation selectors
   U+FE00–U+FE0F and U+E0100–U+E01EF; emoji modifiers U+1F3FB–U+1F3FF; U+20E3 keycap.
2. **ZWJ** U+200D joins the following code point (and its Extend/ZWJ sequence) into the cluster,
   covering emoji ZWJ sequences (family, professions, …).
3. Two **regional indicators** pair into one cluster (flags); a third starts a new cluster.
4. **CRLF** is one cluster (the buffer normalizes CR away anyway).
5. Not modelled: Hangul jamo sequences, Indic conjuncts, spacing marks, prepend characters, and
   the remaining UAX #29 rules. Cluster scanning is O(n) in the buffer; a versioned grapheme index
   is deferred (G-01).

Text validity and normalization: inserted/committed text must be valid UTF-8 (no overlong forms,
surrogates, or > U+10FFFF); invalid input leaves the buffer unchanged with `InvalidUtf8`. CRLF and
lone CR normalize to LF on insert and `set_text`, so the buffer never stores CR.

Word and line rules: word code points are ASCII `[A-Za-z0-9_]` plus every non-ASCII code point;
word-left/right skip non-word code points to the next word boundary. Lines are LF-delimited; line
home is the byte after the previous LF (0 on the first line) and line end is the next LF (size on
the last line).

Bounded length: an insertion that would exceed `max_bytes` is truncated at a cluster boundary
(never inside a cluster) and reports `truncated`; when nothing fits, the buffer is unchanged and the
status is `Truncated`. The grapheme `maxLength` whole-reject policy from section 7 is deferred.

Controller and event wiring (`EditController`):

- Owns one `EditBuffer` plus the IME composition state. The preedit text is **not** in the buffer
  until committed; `CompositionStart` records the range to replace (the selection at start),
  `CompositionUpdate` stores the bounded preedit (invalid UTF-8 rejected), `EditText` commits by
  `replace_range` (or inserts at the caret when no composition is active), and `CompositionEnd`
  without a commit **cancels** and leaves text/selection untouched.
- `handle_event(Event&)` consumes `Text` (one code point; CR/control characters ignored, LF only in
  multi-line fields), `EditText`, the composition family, and the editing `KeyDown` keys:
  Backspace/Delete (Ctrl = word), Left/Right (Ctrl = word, Shift = extend), Home/End (Ctrl =
  document, Shift = extend), Ctrl+A select-all, and Enter (multi-line: inserts `"\n"`; single-line:
  not consumed, submit is deferred). Consumed events get `event.handled = true`.
- `prevent_default` is honoured first: an event that already carries `prevent_default` or `handled`
  is ignored, so a custom element suppresses the built-in editor by setting either flag before
  forwarding the focused node's events (or by not forwarding them). The editor never sets
  `prevent_default`; pointer/focus/capture behavior is unchanged.
- Read-only fields ignore text/IME commits and mutating keys but still consume navigation and
  selection commands. `set_text` is the controlled-value sync point (cancels any composition);
  `last_result()`/`take_changed()` feed the `onChange` wiring (typing coalescing policy is a later
  form-layer concern).

Determinism and tests: no clock, locale, fonts, or random state; the same command/event script
produces byte-identical text, caret, anchor, and composition state. Coverage in
`tests/unit/text_edit_tests.cpp` (`text_edit`) and `tests/unit/text_edit_controller_tests.cpp`
(`text_edit_controller`): insertion/removal at boundaries, composed vs decomposed accents, ZWJ and
variation-selector clusters, flag pairs, CRLF normalization, invalid UTF-8, selection replacement,
word/line movement, selection collapse/extension, clamping, bounded-length truncation, reserve,
composition commit/cancel/invalid preedit, key commands, read-only, suppress-by-`prevent_default`,
controlled sync, determinism, and a scripted sequence with an exact final text/caret.

Demo integration (editable field, implemented): the headless generated-UI demo
(`tests/integration/generated_ui_tests.cpp`) drives a scripted sequence through `EditController`
(type, Home/End/arrows, Shift-selection, select-all, delete-selection) and publishes the buffer as
`PaintData::text` plus `PaintData::decoration` (`caret_byte_offset`, half-open selection bytes,
`caret_visible`; blink is a paint input, there is no timer). Paint resolves the byte offsets to
line-box rectangles through the configured metrics source; the demo uses the real-face source
(`FontPaintTextMetrics`) so caret/selection geometry matches the shaped glyphs, and asserts the
rects stay inside the field's content box, the caret sits at the right edge of a half-open
selection, and deleting the selection collapses it. Current paint limitation found here: an empty
text run paints no decoration, so an empty focused field has no visible caret until the paint pass
grows an empty-run caret.

Runtime change events (implemented, task-63): `TextInputRuntime`
([`text_input_runtime.hpp`](../../include/tessera/ui/text/text_input_runtime.hpp), library
`tessera_text_edit`) wires one `EditController` to the scene node that owns the field:

```cpp
editor.attach(node);                 // node outlives the runtime; nullptr detaches
if (runtime.handle_event(event)) {   // forward the node's on_event input
    event.handled = true;
}
```

- **Dedicated event type:** committed edits dispatch `EventType::TextChange`, not a second
  `EditText`. `EditText` remains the platform/IME commit event (raw handlers and recording sinks see
  it once); re-dispatching `EditText` would re-run those handlers for the same commit. The dedicated
  type lets a generated `onChange` bind only committed edits and keeps the existing route log
  unchanged when nothing is bound.
- **When:** exactly one event per committed text mutation - `Text` insertion, an `EditText`/IME
  commit, or a controller mutation such as Backspace/Delete/paste. Preedit `CompositionUpdate`s,
  caret/selection moves, the external controlled-value `set_text` sync, unfocused fields, read-only
  controllers and detached/destroyed targets emit nothing. Auto-repeat is not a separate rule: only
  an actual mutation emits, so a repeated arrow is silent while a repeated Backspace that really
  deletes emits one event for that deletion.
- **Route:** the runtime calls the scene's `dispatch_event_to` from the target, so the change event
  is delivered synchronously through the normal capture -> target -> bubble path. It is dispatched
  from inside the input handler; a change handler must not mutate the same controller re-entrantly
  (that would invalidate the borrowed value view and recurse).
- **Payload:** `TextChangeEvent { value, target_value, selection_start, selection_end }`.
  `value`/`target_value` are the new full UTF-8 text, **borrowed** from the edit buffer and valid
  only for the duration of the handler (copy it to store). `Event::text` additionally carries a
  bounded inline copy (`kMaxEditTextBytes` = 256 bytes, `text_truncated` when cut) for recording
  sinks. `selection_start`/`selection_end` are **half-open UTF-8 byte offsets**, the units
  `PaintData::TextDecoration` consumes; `EditText`/IME events keep their UTF-16 code-unit selection
  (documented difference). The schema `TextSelection` row still says grapheme offsets and must be
  aligned to UTF-8 bytes in the schema slice.
- **Handled:** handlers set `handled` through the event dispatch; the runtime marks the change event
  handled when the target has a `TextChange` binding.
- **Adapter:** `TextChangeBinding { void (*handler)(void*, const TextChangeEvent&); void* context; }`
  plus `bind_text_change(node, binding)` (a template over the node's `bind_event`, so codegen does
  not include the node header from the payload header and never sees the trampoline). Codegen emits
  one binding member per handler and a `void on_change(const TextChangeEvent&)` method. The binding
  must outlive the node.
- **Bounded:** the runtime allocates nothing per event; the change event only copies the bounded
  inline 256-byte view, and the full value stays a borrowed view into the controller buffer.

Caret blink (implemented, task-67): `CaretBlink`
([`caret_blink.hpp`](../../include/tessera/ui/text/caret_blink.hpp), library `tessera_text_edit`)
is the deterministic blink scheduler. Paint keeps no clock; the fixed-step owner advances the blink
and publishes the result into the paint decoration:

```cpp
CaretBlink blink;                                   // 500/500 ms defaults, configurable
publish_caret_visibility(*paint_data.decoration, blink);  // caret_visible + blink_phase
```

- **Phases:** visible for `visible_seconds` then hidden for `hidden_seconds`, repeating. The visible
  window is half-open `[0, visible_seconds)` and the hidden window is
  `[visible_seconds, visible_seconds + hidden_seconds)`: a tick at exactly the visible boundary is
  hidden, and a tick at exactly the period boundary is visible again. `tick(now_seconds)` takes a
  monotonic time source (a backwards timestamp is clamped, never replayed) and a tick that jumps
  several phases flips at most once - callers that need every transition tick at least once per
  phase.
- **Reset on activity:** typing, a caret move or a selection change calls `reset(now)`, which shows
  the caret immediately (when the field is active) and restarts the cycle. The owner detects
  activity by comparing `EditController::activity_count()`, which counts every command that changed
  the text, selection or caret.
- **Hide rules:** a non-collapsed selection hides the caret (platform-typical for Windows, macOS and
  Linux editors), as do an unfocused field and a read-only field.
  `EditController::caret_blink_active(focused)` computes `focused && !read_only &&
  !has_selection`; the owner feeds it to `CaretBlink::set_active(active, now)`, which hides or shows
  the caret immediately on a context change.
- **Repaint policy:** `tick()`, `reset()` and `set_active()` return true **only** when the effective
  visibility changed, so there are exactly two transitions per period and no repaint churn while
  hidden. Those flags are the only reason to repaint for the caret;
  `next_transition_seconds()` exposes the next exact boundary for on-demand frame scheduling.
- **Publishing and paint:** `publish_caret_visibility(decoration, blink)` writes
  `PaintData::TextDecoration::caret_visible` and `blink_phase` consistently with the paint rule
  (`caret_visible && blink_phase >= 0.5f`), so paint reads one coherent snapshot and stays
  timer-free.
- **Damage:** only the caret's visibility changes, never the text layout. The current damage pass
  damages the whole text node, which is conservative for a caret-only change; tighter caret-rect
  damage needs a paint-side caret rect and is deferred.

Deferred: composition/preedit rendering (blink landed, task-67), undo/redo (grouping/caps), clipboard
(copy/cut/paste and secure redaction), editable-node scene model (`TextInputKind`, value/selection
props and generated events), bidi caret affinity and visual line movement (up/down), grapheme
`maxLength` whole-reject, dead-key composition beyond the platform path, caret rect/candidate-window
positioning, selection handles/touch gestures, and accessibility snapshot wiring for value/
selection/caret.

Paint exposure (task-58): the editing layer's caret and selection byte offsets are consumed through
`PaintData::TextDecoration`; paint resolves them through the same metrics/line-box path as wrap and
emits selection fills behind glyphs, then the caret on top (clipped by node/scroll/damage clips,
opacity folded). No timers or edit-buffer access live in paint. See `paint-pass.md` section 10.2.
