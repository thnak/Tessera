## Canvas text: Shift+click, double and triple click, instance TextInput test, mobile Backspace sentinel

Refs #1015

### What changed
- Shift+press in a focused canvas text field extends the selection from the existing anchor; a second press selects the word (a run of word, whitespace or punctuation characters; non-ASCII bytes count as word so UTF-8 never splits), a third selects the whole value. The pointer export gains optional `modifiers` (bit 0 Shift) and `clicks` arguments; callers that omit them get a plain press.
- The page counts consecutive presses itself (`PointerEvent.detail` is 0 for pointer events): same place within 5 px and 500 ms (`createClickCounter` in `canvas-pointer.ts`).
- Mobile Backspace: the hidden textarea holds a zero-width space (`SENTINEL`) while a field is focused, so a soft keyboard always has something to delete and `deleteContentBackward` arrives. All inserts are prevented, so it never reaches the engine; it is restored after each commit and cleared on blur.
- New test: a `TextInput` inside a component instance (onChange stores into the instance's own scope, label and controlled field follow, editing keys work).
- `canvas-pointer` added as an SSR entry so the Node test can import the press counter.
- Docs: `canvas-text-input.md` (decisions, section 3, tests, section 8) and the export table in `canvas-input-session.md`.

### Verification
- Windows vs2026 Release: `scene_canvas_text`, `scene_canvas_text_caret`, `scene_canvas_pointer`, `scene_canvas_session` pass.
- `node scripts/ci/preview_canvas_text.mjs` all pass (sentinel, press counter, updated composition case).
- `python -B scripts/gates.py --quiet`: 15/15. clang-format 22.1.3 clean on every changed C++ file.
- Not run here: the Wasm build of `wasm_canvas_entry.cpp` (emscripten only; the change is the export signature plus forwarding two values) and `preview_browser_canvas_pointer.mjs` (needs a browser dependency). CI Pages lane covers both.

### What is left
- The real-browser IME pass (Chrome, Firefox, Safari, one phone) stays a manual gate, including how a real phone keyboard treats the sentinel. Record versions in the PR that touches this path. #1015 stays open for that only.
