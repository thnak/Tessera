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
Editable-node scene integration, caret/selection rendering, undo/redo, clipboard, bidi visual
movement, candidate-window positioning, and IME producers beyond Win32 remain future work.

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

Deferred: caret/selection/composition rendering and blink, undo/redo (grouping/caps), clipboard
(copy/cut/paste and secure redaction), editable-node scene model (`TextInputKind`, value/selection
props and generated events), bidi caret affinity and visual line movement (up/down), grapheme
`maxLength` whole-reject, dead-key composition beyond the platform path, caret rect/candidate-window
positioning, selection handles/touch gestures, and accessibility snapshot wiring for value/
selection/caret.
