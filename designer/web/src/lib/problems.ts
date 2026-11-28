// SPDX-License-Identifier: MIT

// The Problems model (owner decision, studio focus pass): what the Problems panel lists and what the status bar
// counts, derived from the diagnostics session's own document (`docs/specs/compiler-diagnostics-v1.md`). The
// session's output (its list, status line and JSON) is unchanged; it only hands the diagnostics array to the
// store after each compile, and every number here is computed from each entry's `severity`.
//
// Pure and DOM-free so a Node gate imports the shipped source (`dist-ssr/problems.js`). The store is a tiny
// external store: React reads it with `useSyncExternalStore`, and the Problems panel subscribes only while it is
// visible, so a hidden panel costs one pointer assignment per compile and nothing else.

import type { Diagnostic } from "../views/diagnostics-render";

export type ProblemSeverity = "error" | "warning" | "info";

export const SEVERITIES: readonly ProblemSeverity[] = ["error", "warning", "info"];

/** One row of the panel. `offset` is `null` when the compiler gave no byte offset (the row then jumps by line). */
export interface Problem {
  key: number;
  severity: ProblemSeverity;
  code: string;
  message: string;
  file: string;
  line: number;
  column: number;
  offset: number | null;
  length: number;
}

export interface ProblemsSnapshot {
  /** `false` until the first compile reports (the status bar says "not run yet", the panel says so too). */
  ran: boolean;
  problems: readonly Problem[];
  errors: number;
  warnings: number;
  infos: number;
}

/** The spec's two severities, plus `info` for any informational entry a later compiler adds. */
export function severityOf(raw: string | undefined): ProblemSeverity {
  if (raw === "warning") {
    return "warning";
  }
  if (raw === "info" || raw === "note" || raw === "hint") {
    return "info";
  }
  return "error";
}

function count(value: number | undefined, fallback: number): number {
  return typeof value === "number" && Number.isFinite(value) && value >= 0 ? value : fallback;
}

/** The rows for a diagnostics array, in the compiler's order (nothing is sorted or dropped). */
export function toProblems(diagnostics: readonly Diagnostic[]): Problem[] {
  return diagnostics.map((diagnostic, index) => {
    const span = diagnostic.span;
    return {
      key: index,
      severity: severityOf(diagnostic.severity),
      code: diagnostic.code ?? "",
      message: diagnostic.message ?? "",
      file: span?.file ?? "",
      line: count(span?.line, 0),
      column: count(span?.column, 0),
      offset: typeof span?.offset === "number" && span.offset >= 0 ? span.offset : null,
      length: count(span?.length, 0),
    };
  });
}

export const EMPTY_SNAPSHOT: ProblemsSnapshot = Object.freeze({
  ran: false,
  problems: Object.freeze([]) as readonly Problem[],
  errors: 0,
  warnings: 0,
  infos: 0,
});

/** The rows of one file, with that file's own counts (the group header's numbers). */
export interface ProblemGroup {
  /** The project path; `""` for rows the compiler gave no file (listed last as "(no file)"). */
  file: string;
  problems: Problem[];
  errors: number;
  warnings: number;
  infos: number;
}

/** File path order (rows with no file last), then line, column, code and message: stable whatever order they arrived in. */
export function compareProblems(a: Problem, b: Problem): number {
  if ((a.file === "") !== (b.file === "")) {
    return a.file === "" ? 1 : -1;
  }
  return (
    (a.file < b.file ? -1 : a.file > b.file ? 1 : 0) ||
    a.line - b.line ||
    a.column - b.column ||
    (a.code < b.code ? -1 : a.code > b.code ? 1 : 0) ||
    (a.message < b.message ? -1 : a.message > b.message ? 1 : 0)
  );
}

/** The rows grouped by file, files in path order and each file's rows in position order. */
export function groupByFile(problems: readonly Problem[]): ProblemGroup[] {
  const groups: ProblemGroup[] = [];
  for (const problem of [...problems].sort(compareProblems)) {
    let group = groups[groups.length - 1];
    if (group === undefined || group.file !== problem.file) {
      group = { file: problem.file, problems: [], errors: 0, warnings: 0, infos: 0 };
      groups.push(group);
    }
    group.problems.push(problem);
    if (problem.severity === "error") {
      group.errors += 1;
    } else if (problem.severity === "warning") {
      group.warnings += 1;
    } else {
      group.infos += 1;
    }
  }
  return groups;
}

/**
 * The compile target's rows and the project-wide rows as one list. The analysis worker reports every file, the
 * compile reports what the analysis does not run yet (lowering), so a finding both name (same file, code and
 * line) is listed once, as the project row. With no project rows the compile rows are returned as they came.
 */
export function mergeProblems(compile: readonly Problem[], project: readonly Problem[] | null): Problem[] {
  if (project === null) {
    return [...compile];
  }
  const seen = new Set(project.map((row) => `${row.file}\0${row.code}\0${row.line}`));
  const extra = compile.filter((row) => !seen.has(`${row.file}\0${row.code}\0${row.line}`));
  return [...project, ...extra].map((row, index) => ({ ...row, key: index }));
}

function snapshotOfRows(problems: Problem[]): ProblemsSnapshot {
  let errors = 0;
  let warnings = 0;
  let infos = 0;
  for (const problem of problems) {
    if (problem.severity === "error") {
      errors += 1;
    } else if (problem.severity === "warning") {
      warnings += 1;
    } else {
      infos += 1;
    }
  }
  return { ran: true, problems, errors, warnings, infos };
}

/** The snapshot for a diagnostics array: the counts are computed from each entry's severity. */
export function snapshotOf(diagnostics: readonly Diagnostic[]): ProblemsSnapshot {
  return snapshotOfRows(toProblems(diagnostics));
}

/** `file:line:col`, or just the file (or `""`) when the compiler gave no position. */
export function locationOf(problem: Problem): string {
  if (problem.file === "") {
    return "";
  }
  return problem.line > 0 ? `${problem.file}:${problem.line}:${Math.max(problem.column, 1)}` : problem.file;
}

/** The rows whose severity is switched on in `shown`. */
export function filterProblems(problems: readonly Problem[], shown: ReadonlySet<ProblemSeverity>): Problem[] {
  return problems.filter((problem) => shown.has(problem.severity));
}

const noun = (n: number, one: string): string => `${n} ${n === 1 ? one : `${one}s`}`;

/** The sentence a screen reader gets for the status bar item. */
export function summaryOf(snapshot: ProblemsSnapshot): string {
  if (!snapshot.ran) {
    return "Problems: not run yet";
  }
  const parts = [noun(snapshot.errors, "error"), noun(snapshot.warnings, "warning")];
  if (snapshot.infos > 0) {
    parts.push(`${snapshot.infos} info`);
  }
  return `Problems: ${parts.join(", ")}`;
}

export interface ProblemsStore {
  get(): ProblemsSnapshot;
  /** Replaces the compile rows with a finished compile's diagnostics. */
  publish(diagnostics: readonly Diagnostic[]): void;
  /** Replaces the project-wide rows (the analysis worker's diagnostics for every file); `null` drops them. */
  publishProject(rows: readonly Problem[] | null): void;
  /** Back to "not run yet" (a project closed). */
  reset(): void;
  subscribe(listener: () => void): () => void;
  /** How many listeners are subscribed: the gate's proof that a hidden panel is not one of them. */
  listenerCount(): number;
}

export function createProblemsStore(): ProblemsStore {
  let current: ProblemsSnapshot = EMPTY_SNAPSHOT;
  let compile: Problem[] | null = null;
  let project: readonly Problem[] | null = null;
  const listeners = new Set<() => void>();
  const set = (next: ProblemsSnapshot): void => {
    current = next;
    for (const listener of [...listeners]) {
      listener();
    }
  };
  // The snapshot is "not run yet" until either source has reported once.
  const refresh = (): void =>
    set(compile === null && project === null ? EMPTY_SNAPSHOT : snapshotOfRows(mergeProblems(compile ?? [], project)));
  return {
    get: () => current,
    publish: (diagnostics) => {
      compile = toProblems(diagnostics);
      refresh();
    },
    publishProject: (rows) => {
      project = rows;
      refresh();
    },
    reset: () => {
      compile = null;
      project = null;
      set(EMPTY_SNAPSHOT);
    },
    subscribe(listener) {
      listeners.add(listener);
      return () => void listeners.delete(listener);
    },
    listenerCount: () => listeners.size,
  };
}
