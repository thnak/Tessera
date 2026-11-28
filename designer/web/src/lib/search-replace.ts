// SPDX-License-Identifier: MIT

// The pure half of project replace (#580): applying a replace plan's edits to a file's text, and the
// undo/redo history of multi-file replaces. One replace (a match, a file, all results) is ONE history entry
// that can span many files, so a single Undo restores them all. The page's text editor keeps its own per-file
// history for typing; this history belongs to the Search panel (Edit > Undo is routed by focus, ADR for #585,
// and focus in the panel's own controls reaches the panel's Undo button and its chord).
//
// No DOM, no React: the Node gate imports this module as the page ships it.

/** One edit of a replace plan, in the coordinates of the text the search ran over (UTF-16 offsets). */
export interface TextEdit {
  offset: number;
  length: number;
  newText: string;
}

/** The text after `edits` (all against the original text, in any order). Throws when they overlap or leave the text. */
export function applyOriginalEdits(text: string, edits: readonly TextEdit[]): string {
  const ordered = [...edits].sort((a, b) => a.offset - b.offset);
  let out = "";
  let at = 0;
  for (const edit of ordered) {
    if (edit.offset < at || edit.length < 0 || edit.offset + edit.length > text.length) {
      throw new RangeError("a replace edit overlaps another or lies outside the file");
    }
    out += text.slice(at, edit.offset) + edit.newText;
    at = edit.offset + edit.length;
  }
  return out + text.slice(at);
}

/** One file's change inside a replace: the text before and after. */
export interface FileChange {
  path: string;
  before: string;
  after: string;
}

/** One undoable replace, however many files it touched. */
export interface ReplaceEntry {
  label: string;
  changes: readonly FileChange[];
}

/** What an undo or redo did: the writes to make, and the files left alone because they changed since. */
export interface StepResult {
  label: string;
  writes: { path: string; text: string }[];
  skipped: string[];
}

export interface ReplaceHistory {
  push(entry: ReplaceEntry): void;
  canUndo(): boolean;
  canRedo(): boolean;
  undoLabel(): string;
  redoLabel(): string;
  /** Restores every file that still holds the replaced text; a file edited since is skipped and reported. */
  undo(textOf: (path: string) => string): StepResult | null;
  redo(textOf: (path: string) => string): StepResult | null;
  clear(): void;
}

/** The newest entries kept; an older replace cannot be undone any more. */
export const MAX_REPLACE_HISTORY = 20;

export function createReplaceHistory(): ReplaceHistory {
  let undone: ReplaceEntry[] = [];
  let redone: ReplaceEntry[] = [];

  const step = (
    from: ReplaceEntry[],
    to: ReplaceEntry[],
    textOf: (path: string) => string,
    forward: boolean,
  ): StepResult | null => {
    const entry = from.pop();
    if (entry === undefined) {
      return null;
    }
    const writes: StepResult["writes"] = [];
    const skipped: string[] = [];
    for (const change of entry.changes) {
      const expected = forward ? change.before : change.after;
      if (textOf(change.path) === expected) {
        writes.push({ path: change.path, text: forward ? change.after : change.before });
      } else {
        skipped.push(change.path);
      }
    }
    if (writes.length > 0) {
      to.push(entry);
    }
    return { label: entry.label, writes, skipped };
  };

  return {
    push(entry) {
      undone.push(entry);
      if (undone.length > MAX_REPLACE_HISTORY) {
        undone = undone.slice(-MAX_REPLACE_HISTORY);
      }
      redone = [];
    },
    canUndo: () => undone.length > 0,
    canRedo: () => redone.length > 0,
    undoLabel: () => undone[undone.length - 1]?.label ?? "",
    redoLabel: () => redone[redone.length - 1]?.label ?? "",
    undo: (textOf) => step(undone, redone, textOf, false),
    redo: (textOf) => step(redone, undone, textOf, true),
    clear() {
      undone = [];
      redone = [];
    },
  };
}

/** The history label of a replace: `Replace "a" with "b" (3 in 2 files)`, the strings shortened. */
export function replaceLabel(query: string, replacement: string, count: number, files: number): string {
  const short = (text: string): string => (text.length > 24 ? text.slice(0, 23) + "…" : text);
  return `Replace "${short(query)}" with "${short(replacement)}" (${count} in ${files} ${files === 1 ? "file" : "files"})`;
}
