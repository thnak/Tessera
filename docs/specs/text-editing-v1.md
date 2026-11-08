# Text editing v1

Status: proposed (Big Step B2/C). Decision: ADR-029. Companion to
[`text.md`](../design/text.md), [`input.md`](../design/input.md),
[`forms-and-validation-v1.md`](forms-and-validation-v1.md), and
[`accessibility-v1.md`](accessibility-v1.md).

Text editing is a core primitive, not an app-level widget. It must work with shaping, bidi,
IME, accessibility, clipboard, selection, undo/redo, and deterministic tests.

## 1. Scope

- Single-line `TextInput`, multi-line `TextArea`.
- Caret, selection, composition, editing commands, undo/redo, clipboard.
- Keyboard/mouse/touch/IME/autofill integration.
- Accessibility and headless tests.
- Rich text is out of scope for v1; text/plain is the baseline.

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

- The HAL normalizes composition start/update/end, committed text, dead keys, and candidate
  window rects (see [`input.md`](../design/input.md)).
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
- Redo is cleared by a new edit; external value changes reset history (documented).
- History is never persisted by default; state restoration may opt in for drafts (see
  [`state-restoration-v1.md`](state-restoration-v1.md)).

## 6. Clipboard

- V1: text/plain; HTML/RTF/file lists are later.
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
- Validation/errors are form concerns, not text editing; see forms spec.

## 8. Layout, paint, and accessibility

- Caret blink uses the runtime clock; deterministic mode pins it off/on for goldens.
- Selection highlight follows visual runs (bidi), not byte ranges.
- Composition underline, caret, and selection are paint primitives; no platform draw calls.
- Text scale/font fallback reflows; caret/selection hit testing uses shaped clusters.
- Scroll caret into view uses the scroll system; no layout feedback loop.
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

## 11. Open questions

- UTF-8 grapheme offsets vs opaque handles for very large documents.
- Rich text/HTML clipboard and paste sanitization.
- Spellcheck/autocorrect integration and privacy.
- IME candidate-window geometry on multi-monitor/foldables.
- Undo persistence for drafts.
- Multi-caret/column selection (not v1).
