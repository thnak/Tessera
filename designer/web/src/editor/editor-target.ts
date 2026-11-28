// SPDX-License-Identifier: MIT

// The editor commands the workbench routes to the focused editor (`studio-workbench-v1.md` section 2.3,
// "Focus-routed editor commands"): undo, redo, select all, find and replace, comment, clipboard and go to
// line. CodeMirror implements the target today; the registry only knows command ids, so a menu click, a
// palette entry and a chord all end here.
//
// The target finds the view from the DOM (`EditorView.findFromDOM` on the content element `#source`), so
// the editor module keeps its small handle and nothing here reaches into `CodeRegion`.

import { selectAll, toggleComment } from "@codemirror/commands";
import { findNext, findPrevious, openSearchPanel } from "@codemirror/search";
import { EditorView } from "@codemirror/view";
import { formatOnSave } from "../lib/format-prefs";
import { analysisHub, type Position } from "./analysis-hub";
import { formatView, isFormattable } from "./format-document";

type BeforeFormat = (path: string, text: string) => void;
const beforeFormat = new Set<BeforeFormat>();

/** Version history (#583) subscribes here to keep the text a format is about to rewrite; returns the unsubscribe. */
export function onBeforeFormat(listener: BeforeFormat): () => void {
  beforeFormat.add(listener);
  return () => void beforeFormat.delete(listener);
}

function announceBeforeFormat(view: EditorView): void {
  const path = view.contentDOM.dataset.path ?? "";
  if (!isFormattable(path)) {
    return;
  }
  for (const listener of beforeFormat) {
    listener(path, view.state.doc.toString());
  }
}

/**
 * Applies a restore edit to the open file as ONE transaction (one Undo step). Returns false when the editor shows
 * another file, so the caller can fall back to writing the buffer.
 */
export function applyEditorEdit(path: string, edit: { from: number; to: number; insert: string }): boolean {
  const view = viewOf();
  if (view === null || (view.contentDOM.dataset.path ?? "") !== path || edit.to > view.state.doc.length) {
    return false;
  }
  view.dispatch({ changes: edit, userEvent: "input.restore", scrollIntoView: true });
  return true;
}

function viewOf(): EditorView | null {
  const content = document.getElementById("source");
  return content === null ? null : EditorView.findFromDOM(content);
}

/** The editor's state facts the workbench publishes as context keys. */
export function editorFacts(): { focus: boolean; hasSelection: boolean } {
  const view = viewOf();
  if (view === null) {
    return { focus: false, hasSelection: false };
  }
  return { focus: view.hasFocus, hasSelection: !view.state.selection.main.empty };
}

function withView(action: (view: EditorView) => void): void {
  const view = viewOf();
  if (view === null) {
    throw new Error("no editor is open");
  }
  view.focus();
  action(view);
}

/** The caret of the open editor as an analysis request position. */
function caretPosition(): Position {
  const view = viewOf();
  if (view === null) {
    throw new Error("no editor is open");
  }
  view.focus();
  return { path: view.contentDOM.dataset.path ?? "", text: view.state.doc.toString(), offset: view.state.selection.main.head };
}

/** The commands, keyed by workbench command id. Each focuses the editor first: a menu click took focus. */
export const EDITOR_COMMANDS: Record<string, () => void | Promise<void>> = {
  // `edit.undo` / `edit.redo` are routed by focus (`page-edit-routing.ts`), so they are not editor-only.
  "edit.selectAll": () => withView((view) => void selectAll(view)),
  "edit.find": () => withView((view) => void openSearchPanel(view)),
  "edit.replace": () => withView((view) => void openSearchPanel(view)),
  "edit.findNext": () => withView((view) => void findNext(view)),
  "edit.findPrevious": () => withView((view) => void findPrevious(view)),
  "edit.format": async () => {
    const view = viewOf();
    if (view === null) {
      throw new Error("no editor is open");
    }
    view.focus();
    announceBeforeFormat(view);
    const report = await formatView(view);
    if (report.outcome === "refused") {
      // A thrown error becomes the command's failure toast; the text was left untouched.
      throw new Error(`Format Document refused the file: ${report.message}`);
    }
    if (report.outcome === "skipped" && !isFormattable(view.contentDOM.dataset.path ?? "")) {
      throw new Error("Format Document formats .tsx, .css and tessera.json files");
    }
  },
  "go.definition": () => analysisHub.definition(caretPosition()),
  "go.references": () => analysisHub.references(caretPosition()),
  "edit.renameSymbol": () => analysisHub.rename(caretPosition()),
  "edit.undoRename": () => analysisHub.undoRename(),
  "edit.toggleComment": () => withView((view) => void toggleComment(view)),
  "edit.cut": () => withView(() => void document.execCommand("cut")),
  "edit.copy": () => withView(() => void document.execCommand("copy")),
  "edit.paste": async () => {
    const view = viewOf();
    if (view === null) {
      throw new Error("no editor is open");
    }
    view.focus();
    const text = await navigator.clipboard.readText();
    view.dispatch({ ...view.state.replaceSelection(text), userEvent: "input.paste", scrollIntoView: true });
  },
};

/**
 * Format-on-save (#577): formats the open file before it is written when the setting is on. A refusal or a
 * worker failure never blocks the save; the text is saved as the person left it.
 */
export async function formatBeforeSave(): Promise<void> {
  const view = viewOf();
  if (view === null || !formatOnSave()) {
    return;
  }
  try {
    announceBeforeFormat(view);
    if ((await formatView(view)).outcome === "formatted") {
      // Let the editor's change reach the page state (React) before the save reads it.
      await new Promise((resolve) => setTimeout(resolve, 0));
      await new Promise((resolve) => setTimeout(resolve, 0));
    }
  } catch {
    // The save goes on with the unformatted text.
  }
}

/** The editor's line count, for the go-to-line prompt's range; `0` with no editor. */
export function editorLineCount(): number {
  return viewOf()?.state.doc.lines ?? 0;
}

/** The caret's 1-based line and column, for the status line and the go-to-line prompt. */
export function editorCaret(): { line: number; column: number } | null {
  const view = viewOf();
  if (view === null) {
    return null;
  }
  const head = view.state.selection.main.head;
  const line = view.state.doc.lineAt(head);
  return { line: line.number, column: head - line.from + 1 };
}

/** Moves the caret to a 1-based line (clamped) and scrolls it to the centre. */
export function editorGotoLine(line: number): void {
  withView((view) => {
    const target = view.state.doc.line(Math.min(Math.max(1, Math.floor(line)), view.state.doc.lines));
    view.dispatch({ selection: { anchor: target.from }, effects: EditorView.scrollIntoView(target.from, { y: "center" }) });
  });
}
