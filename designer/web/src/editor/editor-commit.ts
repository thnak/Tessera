// SPDX-License-Identifier: MIT

// The editor's half of a style commit and of the text history the Edit menu names (issue #585, rule D3).
//
// A commit lands in the open file as ONE CodeMirror transaction, which is one text-history entry:
// `isolateHistory` keeps it from merging with typing on either side, so Ctrl+Z in the editor reverts exactly
// the commit. The transaction carries `quietCommit`, which `code-editor.ts` reads to skip its `onChange`: the
// page writes the buffer and evaluates itself, in the order the round trip needs (apply, evaluate, settle),
// instead of letting the keystroke path schedule a debounced apply (or, for a non-source file, a restart).

import { isolateHistory, redo, redoDepth, undo, undoDepth } from "@codemirror/commands";
import { Annotation, type ChangeSpec } from "@codemirror/state";
import { EditorView } from "@codemirror/view";
import type { StackFacts } from "../lib/edit-routing";

/** Marks the transaction a style commit made; `code-editor.ts` does not report it as a keystroke. */
export const quietCommit = Annotation.define<boolean>();

function viewOf(): EditorView | null {
  // The Node gates render the chrome on the server, where there is no document and so no editor.
  if (typeof document === "undefined") {
    return null;
  }
  const content = document.getElementById("source");
  return content === null ? null : EditorView.findFromDOM(content);
}

// The undo depth right after the last commit, and its label: while the history is still that deep the top
// entry IS the commit, so the menu says `Undo Commit style to Button`; any later typing says `Undo typing`.
let lastCommit: { depth: number; label: string } | null = null;

/** The text history's facts for the Edit menu; empty when no editor is mounted. */
export function textStack(): StackFacts {
  const view = viewOf();
  if (view === null) {
    return { canUndo: false, canRedo: false, undoLabel: "", redoLabel: "" };
  }
  const depth = undoDepth(view.state);
  const redone = redoDepth(view.state);
  const isCommit = (at: number): boolean => lastCommit !== null && at === lastCommit.depth;
  return {
    canUndo: depth > 0,
    canRedo: redone > 0,
    undoLabel: isCommit(depth) ? (lastCommit?.label ?? "typing") : "typing",
    redoLabel: isCommit(depth + 1) ? (lastCommit?.label ?? "typing") : "typing",
  };
}

/** Undoes in the editor's own history; returns whether it changed the document. */
export function undoText(): boolean {
  const view = viewOf();
  if (view === null) {
    return false;
  }
  view.focus();
  return undo(view);
}

export function redoText(): boolean {
  const view = viewOf();
  if (view === null) {
    return false;
  }
  view.focus();
  return redo(view);
}

/**
 * Applies a commit's changes to the open document as one history entry and returns the new text, or
 * `null` when no editor is mounted. The caller names the open file; this never guesses which file is shown.
 */
export function commitToEditor(changes: ChangeSpec, label: string): string | null {
  const view = viewOf();
  if (view === null) {
    return null;
  }
  view.dispatch({
    changes,
    annotations: [quietCommit.of(true), isolateHistory.of("full")],
    userEvent: "input.commit",
  });
  lastCommit = { depth: undoDepth(view.state), label };
  return view.state.doc.toString();
}
