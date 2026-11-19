// SPDX-License-Identifier: MIT

// The editor commands the workbench routes to the focused editor (`studio-workbench-v1.md` section 2.3,
// "Focus-routed editor commands"): undo, redo, select all, find and replace, comment, clipboard and go to
// line. CodeMirror implements the target today; the registry only knows command ids, so a menu click, a
// palette entry and a chord all end here.
//
// The target finds the view from the DOM (`EditorView.findFromDOM` on the content element `#source`), so
// the editor module keeps its small handle and nothing here reaches into `CodeRegion`.

import { redo, selectAll, toggleComment, undo } from "@codemirror/commands";
import { findNext, findPrevious, openSearchPanel } from "@codemirror/search";
import { EditorView } from "@codemirror/view";

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

/** The commands, keyed by workbench command id. Each focuses the editor first: a menu click took focus. */
export const EDITOR_COMMANDS: Record<string, () => void | Promise<void>> = {
  "edit.undo": () => withView((view) => void undo(view)),
  "edit.redo": () => withView((view) => void redo(view)),
  "edit.selectAll": () => withView((view) => void selectAll(view)),
  "edit.find": () => withView((view) => void openSearchPanel(view)),
  "edit.replace": () => withView((view) => void openSearchPanel(view)),
  "edit.findNext": () => withView((view) => void findNext(view)),
  "edit.findPrevious": () => withView((view) => void findPrevious(view)),
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
