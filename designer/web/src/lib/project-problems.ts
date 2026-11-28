// SPDX-License-Identifier: MIT

// The project-wide Problems source (#575, ADR-060): what the page sends the analysis worker and how its
// `diagnostics` answer becomes Problems rows. Pure and DOM-free so a Node gate imports the shipped source
// (`dist-ssr/project-problems.js`) and drives it against the real worker protocol.
//
// The worker answers in UTF-16 spans without a line (`docs/specs/studio-services-v1.md` section 11); the rows
// need `file:line:col` for the list and the status bar, and the UTF-16 offset is exactly what the editor's
// `reveal` takes, so a click lands on the span even after non-BMP text.

import type { Problem } from "./problems";
import { severityOf } from "./problems";

/** One analysis snapshot entry: `[file id, text, project path]`. The id is the index, so it is stable per list. */
export type AnalysisEntry = [number, string, string];

/** A `Diagnostic` of section 11 (the file is implied by the entry that holds it). */
export interface AnalysisDiagnostic {
  code: string;
  severity: string;
  message: string;
  from: number;
  to: number;
}

export interface AnalysisDiagnostics {
  files: { file: number; diagnostics: AnalysisDiagnostic[] }[];
}

/** The files the analysis worker reads: sources (parsed) and stylesheets (so a relative import resolves). */
export function analysisEntries(
  files: readonly { path: string; kind: string }[],
  textOf: (path: string) => string,
): AnalysisEntry[] {
  return files
    .filter((file) => file.kind === "source" || file.kind === "stylesheet")
    .map((file, index): AnalysisEntry => [index, textOf(file.path), file.path]);
}

/** 1-based line and column (UTF-16 units) of `offset` in `text`; an offset past the end clamps to the end. */
export function lineColumnOf(text: string, offset: number): { line: number; column: number } {
  const end = Math.min(Math.max(offset, 0), text.length);
  let line = 1;
  let lineStart = 0;
  for (let at = text.indexOf("\n"); at !== -1 && at < end; at = text.indexOf("\n", at + 1)) {
    line += 1;
    lineStart = at + 1;
  }
  return { line, column: end - lineStart + 1 };
}

/** The Problems rows for an analysis answer; `entries` are the ones the snapshot was built from. */
export function problemsFromAnalysis(result: AnalysisDiagnostics, entries: readonly AnalysisEntry[]): Problem[] {
  const byId = new Map(entries.map((entry) => [entry[0], entry]));
  const rows: Problem[] = [];
  for (const group of result.files) {
    const entry = byId.get(group.file);
    if (entry === undefined) {
      continue;
    }
    for (const diagnostic of group.diagnostics) {
      const { line, column } = lineColumnOf(entry[1], diagnostic.from);
      rows.push({
        key: rows.length,
        severity: severityOf(diagnostic.severity),
        code: diagnostic.code,
        message: diagnostic.message,
        file: entry[2],
        line,
        column,
        offset: diagnostic.from,
        length: Math.max(diagnostic.to - diagnostic.from, 0),
      });
    }
  }
  return rows;
}
