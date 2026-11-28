// SPDX-License-Identifier: MIT

// The pure half of the version history panel (#583): what a timeline row says, how far a snapshot is from the
// working text, and what restoring one does. No DOM and no React: `scripts/ci/preview_history_model.mjs` imports
// this module as the page ships it.
//
// **Restore never loses work.** The working text of a file that differs from the snapshot is first recorded as a
// 'restore' snapshot, then the snapshot's text is applied as ONE minimal edit (`restoreEdit`) the editor
// dispatches as a single transaction, so one Undo returns the working text.

import { diffLines } from "../../line-diff.js";
import { restoreEdit, timeline, type Snapshot } from "../../snapshots.js";

export type { Snapshot };

/** The minimal replacement and the text it must produce, in UTF-16 offsets of the working text. */
export interface RestoreEdit {
  from: number;
  to: number;
  insert: string;
}

const REASON_LABELS: Record<string, string> = {
  save: "Saved",
  format: "Before format",
  rename: "Before rename",
  restore: "Before restore",
};

/** The sentence for why a version exists; an unknown reason (a future one) is shown as it is. */
export function reasonLabel(reason: string): string {
  return REASON_LABELS[reason] ?? reason;
}

/** "just now", "5 min ago", "3 h ago", or the UTC date; `now` is passed in so the result is deterministic. */
export function formatAge(at: number, now: number): string {
  const seconds = Math.max(0, Math.floor((now - at) / 1000));
  if (seconds < 45) {
    return "just now";
  }
  const minutes = Math.round(seconds / 60);
  if (minutes < 60) {
    return `${minutes} min ago`;
  }
  const hours = Math.round(minutes / 60);
  if (hours < 24) {
    return `${hours} h ago`;
  }
  return new Date(at).toISOString().slice(0, 16).replace("T", " ") + " UTC";
}

/** Lines added and removed going from `snapshotText` to `currentText`: what a restore would undo. */
export function changeStat(snapshotText: string, currentText: string): { added: number; removed: number } {
  let added = 0;
  let removed = 0;
  for (const segment of diffLines(snapshotText, currentText)) {
    if (segment.kind === "insert") {
      added += segment.lines.length;
    } else if (segment.kind === "delete") {
      removed += segment.lines.length;
    }
  }
  return { added, removed };
}

export interface TimelineRow {
  id: string;
  label: string;
  age: string;
  bytes: number;
  /** The snapshot equals the working text: nothing to restore. */
  current: boolean;
  /** Lines the working text has that the snapshot lacks, and the reverse. */
  added: number;
  removed: number;
}

/** The rows of one file's timeline, newest first, each compared with the working text. */
export function describeTimeline(
  history: { entries: readonly Snapshot[] },
  path: string,
  currentText: string,
  now: number,
): TimelineRow[] {
  return timeline(history as never, path).map((entry: Snapshot) => {
    const current = entry.text === currentText;
    const stat = current ? { added: 0, removed: 0 } : changeStat(entry.text, currentText);
    return { id: entry.id, label: reasonLabel(entry.reason), age: formatAge(entry.at, now), bytes: entry.bytes, current, ...stat };
  });
}

export interface DiffLine {
  kind: "insert" | "delete";
  /** 1-based line in the snapshot (delete) or the working text (insert). */
  line: number;
  text: string;
}

/** Cap on the lines the panel draws; the rest are counted, never silently dropped. */
export const MAX_DIFF_LINES = 400;

/** The changed lines from the snapshot to the working text (what a restore reverts), capped. */
export function diffRows(snapshotText: string, currentText: string): { rows: DiffLine[]; hidden: number } {
  const rows: DiffLine[] = [];
  let hidden = 0;
  for (const segment of diffLines(snapshotText, currentText)) {
    if (segment.kind === "equal") {
      continue;
    }
    segment.lines.forEach((text: string, index: number) => {
      if (rows.length >= MAX_DIFF_LINES) {
        hidden += 1;
        return;
      }
      const line = (segment.kind === "delete" ? segment.aLine : segment.bLine) + index + 1;
      rows.push({ kind: segment.kind as "insert" | "delete", line, text: text.replace(/\r?\n$/, "") });
    });
  }
  return { rows, hidden };
}

export interface RestorePlan {
  /** The snapshot equals the working text. */
  noop: boolean;
  edit: RestoreEdit;
  /** Recorded before the edit is applied so the working text is never lost; `null` when there is nothing to keep. */
  keep: { text: string; reason: "restore" } | null;
}

/** What restoring `snapshotText` over `currentText` does. */
export function planRestore(currentText: string, snapshotText: string): RestorePlan {
  if (currentText === snapshotText) {
    return { noop: true, edit: { from: 0, to: 0, insert: "" }, keep: null };
  }
  return { noop: false, edit: restoreEdit(currentText, snapshotText), keep: { text: currentText, reason: "restore" } };
}

/** The newest snapshot of `path` that differs from the working text (Restore Previous Version), or `undefined`. */
export function previousVersion(history: { entries: readonly Snapshot[] }, path: string, currentText: string): Snapshot | undefined {
  return timeline(history as never, path).find((entry: Snapshot) => entry.text !== currentText);
}
