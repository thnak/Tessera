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
see [`input.md`](../design/input.md) sections 2.1 and 8). The editor itself is not implemented:
editable nodes/`TextInputKind`, caret/selection rendering and editing commands, default text
insertion, candidate-window positioning, undo/redo/clipboard, bidi/grapheme logic, and IME
producers beyond Win32 remain future work.

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
