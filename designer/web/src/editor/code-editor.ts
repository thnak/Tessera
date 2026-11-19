// SPDX-License-Identifier: MIT

// The studio's code editor (issue #517): CodeMirror 6 behind a small handle.
//
// **Why a handle and not the view.** The page needs five things from an editor -- show this file's text,
// say when the person typed, report the text on demand, reveal a span, and underline the compiler's
// diagnostics -- and nothing else. Everything CodeMirror-specific stays in this folder, so the regions,
// the sessions and the probes talk to `CodeEditor`, and a later change of editor (or a dock that moves the
// host element) does not reach them.
//
// **One document per open file, history kept.** Each open file keeps its own `EditorState`, so undo works
// after a tab switch and the caret and scroll come back. A page write -- a sample, a restored draft, a
// share link -- is detected by comparing the stored document with the text the page hands over: equal
// means "the person's own edit history is still right" (reuse the state), different means "the page
// replaced the file" (a fresh state, history cleared, which is what the textarea did).
//
// **Typing never goes through React.** The update listener calls `onChange(text)` for a user edit only; a
// programmatic `setState` does not fire it, which is why a tab switch is not mistaken for an edit.
// The keystroke cost is one `onChange` per transaction, as the textarea's `input` event was.
//
// The module touches the DOM only inside `createCodeEditor`, so importing it in Node (the SSR gate) is safe.

import { defaultKeymap, history, historyKeymap } from "@codemirror/commands";
import { css } from "@codemirror/lang-css";
import { javascript } from "@codemirror/lang-javascript";
import { json } from "@codemirror/lang-json";
import {
  bracketMatching,
  defaultHighlightStyle,
  indentOnInput,
  syntaxHighlighting,
} from "@codemirror/language";
import { highlightSelectionMatches, searchKeymap } from "@codemirror/search";
import {
  EditorState,
  StateEffect,
  StateField,
  type Extension,
} from "@codemirror/state";
import {
  Decoration,
  type DecorationSet,
  EditorView,
  drawSelection,
  highlightActiveLine,
  keymap,
  lineNumbers,
} from "@codemirror/view";
import { studioHighlight, studioTheme } from "./editor-theme";

/** The part of an editor the diagnostics list needs: read the text, select a span. */
export interface EditorSurface {
  text(): string;
  /** Selects `length` characters at `offset`, scrolls them into view and focuses the editor. */
  reveal(offset: number, length: number): void;
}

/** One underlined span in the open file. */
export interface EditorMark {
  offset: number;
  length: number;
  severity: "error" | "warning";
  message: string;
}

export interface CodeEditor extends EditorSurface {
  /**
   * Shows `path` with `text`; reuses the file's own state (history) when its document still equals `text`.
   * `readOnly` shows a locked file: the text can be selected and copied but not changed.
   */
  open(path: string, text: string, readOnly?: boolean): void;
  /** Replaces the underlines of `path`; shown immediately when it is the open file. */
  setMarks(path: string, marks: readonly EditorMark[]): void;
  focus(): void;
  /**
   * Replaces the whole document the way typing would (an `input` user event, so `onChange` fires).
   * A seam for the browser probes, which cannot set a value on a CodeMirror surface; the page does not call it.
   */
  edit(next: string): void;
  destroy(): void;
  /** The path of the open file, or `""`. */
  readonly path: string;
}

export interface CodeEditorOptions {
  /** A user edit: the new full text of the open file. */
  onChange: (text: string) => void;
  /** Mod-S. */
  onSave: () => void;
  /** Mod-Enter: apply now, skipping the debounce. */
  onApplyNow: () => void;
  /** The accessible name of the editing surface. */
  label: string;
  /** The caret moved, the selection changed or a file opened: 1-based line and column, and the selected characters. */
  onCaret?: (caret: { line: number; column: number; selected: number }) => void;
}

const setMarksEffect = StateEffect.define<readonly EditorMark[]>();

/** The underline decorations: replaced as a set by `setMarksEffect`, mapped through edits in between. */
const marksField = StateField.define<DecorationSet>({
  create: () => Decoration.none,
  update(value, transaction) {
    let next = value.map(transaction.changes);
    for (const effect of transaction.effects) {
      if (effect.is(setMarksEffect)) {
        const length = transaction.state.doc.length;
        next = Decoration.set(
          effect.value
            .map((mark) => {
              const from = Math.min(Math.max(0, mark.offset), length);
              // An empty span still gets one character so there is something to underline.
              const to = Math.min(length, Math.max(from + 1, from + Math.max(0, mark.length)));
              return from < to
                ? Decoration.mark({
                    class: `cm-diag cm-diag-${mark.severity}`,
                    attributes: { title: mark.message },
                  }).range(from, to)
                : null;
            })
            .filter((range) => range !== null),
          true,
        );
      }
    }
    return next;
  },
  provide: (field) => EditorView.decorations.from(field),
});

/** The language for a path, or no extension for a file kind the editor only shows as text. */
function languageFor(path: string): Extension {
  if (/\.(tsx|jsx|ts|js|mjs)$/i.test(path)) {
    return javascript({ jsx: true, typescript: true });
  }
  if (/\.css$/i.test(path)) {
    return css();
  }
  if (/\.json$/i.test(path)) {
    return json();
  }
  return [];
}

export function createCodeEditor(parent: HTMLElement, options: CodeEditorOptions): CodeEditor {
  const states = new Map<string, EditorState>();
  const marks = new Map<string, readonly EditorMark[]>();
  let current = "";

  const shared: Extension[] = [
    lineNumbers(),
    history(),
    drawSelection(),
    indentOnInput(),
    bracketMatching(),
    highlightActiveLine(),
    highlightSelectionMatches(),
    syntaxHighlighting(studioHighlight),
    syntaxHighlighting(defaultHighlightStyle, { fallback: true }),
    marksField,
    studioTheme,
    EditorView.contentAttributes.of({
      id: "source",
      "aria-label": options.label,
      spellcheck: "false",
      autocapitalize: "off",
      autocorrect: "off",
    }),
    keymap.of([
      {
        key: "Mod-s",
        preventDefault: true,
        run: () => {
          options.onSave();
          return true;
        },
      },
      {
        key: "Mod-Enter",
        preventDefault: true,
        run: () => {
          options.onApplyNow();
          return true;
        },
      },
      // `Mod-g`/`Shift-Mod-g` are find-next here; the workbench binds Ctrl+G to Go to Line (VS Code's chord).
      ...searchKeymap.filter((binding) => binding.key !== "Mod-g" && binding.key !== "Shift-Mod-g"),
      ...historyKeymap,
      ...defaultKeymap,
    ]),
    EditorView.updateListener.of((update) => {
      if (update.docChanged) {
        options.onChange(update.state.doc.toString());
      }
      if (update.docChanged || update.selectionSet) {
        reportCaret(update.state);
      }
    }),
  ];

  const view = new EditorView({ state: EditorState.create({ doc: "" }), parent });

  function reportCaret(state: EditorState): void {
    const { head, from, to } = state.selection.main;
    const line = state.doc.lineAt(head);
    options.onCaret?.({ line: line.number, column: head - line.from + 1, selected: to - from });
  }

  function stateFor(path: string, text: string, readOnly: boolean): EditorState {
    return EditorState.create({
      doc: text,
      extensions: [shared, languageFor(path), readOnly ? EditorState.readOnly.of(true) : []],
    });
  }

  function applyMarks(path: string): void {
    view.dispatch({ effects: setMarksEffect.of(marks.get(path) ?? []) });
  }

  return {
    get path() {
      return current;
    },
    open(path, text, readOnly = false) {
      if (current !== "" && current !== path) {
        states.set(current, view.state);
      }
      const kept = states.get(path);
      const next = kept !== undefined && kept.doc.toString() === text && kept.readOnly === readOnly ? kept : stateFor(path, text, readOnly);
      current = path;
      view.setState(next);
      applyMarks(path);
      reportCaret(view.state);
    },
    setMarks(path, list) {
      marks.set(path, list);
      if (path === current) {
        applyMarks(path);
      }
    },
    text: () => view.state.doc.toString(),
    reveal(offset, length) {
      const size = view.state.doc.length;
      const from = Math.min(Math.max(0, offset), size);
      const to = Math.min(size, from + Math.max(0, length));
      view.dispatch({ selection: { anchor: from, head: to }, effects: EditorView.scrollIntoView(from, { y: "center" }) });
      view.focus();
    },
    focus: () => view.focus(),
    edit(next) {
      view.dispatch({
        changes: { from: 0, to: view.state.doc.length, insert: next },
        userEvent: "input",
      });
    },
    destroy() {
      view.destroy();
      states.clear();
      marks.clear();
    },
  };
}
