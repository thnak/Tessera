// SPDX-License-Identifier: MIT

// The pure half of the editor's code intelligence (#984, #988): turning the `analysis` service answers of
// studio-services-v1 sections 12 and 13 into what the page shows -- a place to jump to, rows of references, a
// hover card, completion items and a multi-file rename plan. File ids are positions in the snapshot the request
// rode on, so every function takes the entries of that request. No DOM, no CodeMirror: a Node gate imports this
// module as the page ships it.

import type { AnalysisEntry } from "./project-problems";
import { lineColumnOf } from "./project-problems";
import { applyOriginalEdits, type FileChange } from "./search-replace";

/** What the page lends: the project's files and their text. `active` overrides one file with the editor's text. */
export interface NavProject {
  files: readonly { path: string; kind: string }[];
  textOf: (path: string) => string;
}

/** The snapshot of the project, the open file taken from the editor (the page buffer may lag a keystroke behind). */
export function navEntries(project: NavProject, active?: { path: string; text: string }): AnalysisEntry[] {
  return project.files
    .filter((file) => file.kind === "source" || file.kind === "stylesheet")
    .map((file, index): AnalysisEntry => [index, active?.path === file.path ? active.text : project.textOf(file.path), file.path]);
}

export const idOfPath = (entries: readonly AnalysisEntry[], path: string): number => entries.find((entry) => entry[2] === path)?.[0] ?? -1;
const entryOf = (entries: readonly AnalysisEntry[], id: number): AnalysisEntry | undefined => entries.find((entry) => entry[0] === id);

/** A span in a project file. */
export interface Target {
  path: string;
  offset: number;
  length: number;
}

interface Span {
  file: number;
  from: number;
  to: number;
}

const targetOf = (entries: readonly AnalysisEntry[], span: Span): Target | null => {
  const entry = entryOf(entries, span.file);
  return entry === undefined ? null : { path: entry[2], offset: span.from, length: span.to - span.from };
};

const UNRESOLVED: Record<string, string> = {
  external: "is declared outside the project (a package or a style sheet)",
  builtin: "is built in",
  intrinsic: "is a platform element",
  needsType: "needs type information the studio does not have yet",
  unbound: "is not declared",
};

/** F12: the declaration, or why there is none. */
export function definitionResult(answer: unknown, entries: readonly AnalysisEntry[]): { target: Target | null; message: string } {
  const result = answer as { definitions?: Span[]; unresolved?: string };
  const target = (result.definitions ?? []).flatMap((span) => targetOf(entries, span) ?? [])[0] ?? null;
  if (target !== null) {
    return { target, message: "" };
  }
  const why = result.unresolved === undefined ? undefined : UNRESOLVED[result.unresolved];
  return { target: null, message: why === undefined ? "No definition found at the caret." : `This name ${why}.` };
}

/** One row of Find All References. */
export interface ReferenceRow extends Target {
  line: number;
  column: number;
  kind: string;
  /** The line the use is on, trimmed. */
  preview: string;
}

export function referenceRows(answer: unknown, entries: readonly AnalysisEntry[]): ReferenceRow[] {
  const rows: ReferenceRow[] = [];
  for (const reference of (answer as { references?: (Span & { kind: string })[] }).references ?? []) {
    const target = targetOf(entries, reference);
    const text = entryOf(entries, reference.file)?.[1] ?? "";
    if (target === null) {
      continue;
    }
    const { line, column } = lineColumnOf(text, target.offset);
    const start = text.lastIndexOf("\n", target.offset - 1) + 1;
    const end = text.indexOf("\n", target.offset);
    rows.push({ ...target, line, column, kind: reference.kind, preview: text.slice(start, end === -1 ? text.length : end).trim().slice(0, 120) });
  }
  return rows;
}

/** The hover card as plain lines (the editor builds text nodes from them: nothing here is markup). */
export interface HoverCard {
  from: number;
  to: number;
  title: string;
  signature: string;
  doc: string;
  problems: string[];
}

export function hoverCard(answer: unknown): HoverCard | null {
  const hover = (answer as { hover?: Record<string, unknown> | null }).hover;
  if (hover === null || hover === undefined) {
    return null;
  }
  const name = String(hover.name ?? "");
  const kind = String(hover.kind ?? "");
  const container = hover.container === undefined ? "" : ` in ${String(hover.container)}`;
  const unresolved = hover.unresolved === undefined ? "" : ` (${UNRESOLVED[String(hover.unresolved)] ?? String(hover.unresolved)})`;
  const diagnostics = (hover.diagnostics ?? []) as { code: string; severity: string; message: string }[];
  return {
    from: Number(hover.from),
    to: Number(hover.to),
    title: name === "" ? "" : `${kind} ${name}${container}${unresolved}`.trim(),
    signature: String(hover.signature ?? ""),
    doc: String(hover.doc ?? ""),
    problems: diagnostics.map((d) => `${d.severity}: ${d.message} (${d.code})`),
  };
}

/** A completion item the editor offers: the symbol kind decides the icon. */
export interface CompletionOption {
  label: string;
  type: string;
  detail?: string;
}

const COMPLETION_TYPE: Record<string, string> = {
  component: "class",
  import: "namespace",
  prop: "property",
  key: "keyword",
  cssClass: "property",
  file: "text",
  bind: "function",
  setter: "function",
  handler: "function",
  hook: "function",
  function: "function",
};

export function completionOptions(answer: unknown): { from: number; to: number; options: CompletionOption[]; context: string } | null {
  const result = answer as { from: number; to: number; context: string; items?: { label: string; kind: string; detail?: string }[] };
  if (!Array.isArray(result.items) || result.items.length === 0) {
    return null;
  }
  return {
    from: result.from,
    to: result.to,
    context: result.context,
    options: result.items.map((item) => ({ label: item.label, type: COMPLETION_TYPE[item.kind] ?? "variable", ...(item.detail === undefined ? {} : { detail: item.detail }) })),
  };
}

/** What a rename does to the project, or the sentence that says why it was refused. */
export type RenamePlan = { ok: true; changes: FileChange[]; edits: number } | { ok: false; message: string };

export function renamePlan(answer: unknown, entries: readonly AnalysisEntry[]): RenamePlan {
  const result = answer as { edits?: { file: number; offset: number; length: number; newText: string }[]; refusal?: { message: string } };
  if (result.refusal !== undefined) {
    return { ok: false, message: result.refusal.message };
  }
  const byFile = new Map<number, { offset: number; length: number; newText: string }[]>();
  for (const edit of result.edits ?? []) {
    byFile.set(edit.file, [...(byFile.get(edit.file) ?? []), edit]);
  }
  const changes: FileChange[] = [];
  try {
    for (const [file, edits] of byFile) {
      const entry = entryOf(entries, file);
      if (entry !== undefined) {
        changes.push({ path: entry[2], before: entry[1], after: applyOriginalEdits(entry[1], edits) });
      }
    }
  } catch {
    return { ok: false, message: "The rename does not fit the files as they are now. Try again." };
  }
  const edits = result.edits?.length ?? 0;
  return changes.length === 0 ? { ok: false, message: "Nothing to rename." } : { ok: true, changes, edits };
}

export function renameLabel(from: string, to: string, edits: number, files: number): string {
  return `Rename ${from} to ${to} (${edits} ${edits === 1 ? "place" : "places"} in ${files} ${files === 1 ? "file" : "files"})`;
}
