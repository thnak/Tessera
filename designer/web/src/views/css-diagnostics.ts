// SPDX-License-Identifier: MIT

// The author stylesheet's diagnostics, as the page shows them (issue #390).
//
// **One module, because two views report the same thing.** The Scene view and the Canvas view both lay
// the source out against the author's CSS, so both can have a bad rule to report. A copy of "call the
// export, parse the document, phrase the sentence" in each session would be two chances to phrase the
// same problem differently, and the page has one diagnostics area a visitor reads.
//
// **Why a separate export and not a field of the snapshot.** The snapshot JSON is the P2 parity
// artifact and `scene_snapshot_parity.py` pins its bytes; a stylesheet problem is not part of the tree.
// `tessera_css_diagnostics_json` is the scene module's own answer, and it is absent from an older
// module, which is a state this module handles rather than throws on.
//
// **A bad rule never blanks the frame.** The stylesheet parser is tolerant, so these are reported and
// the tree is drawn from the rules that did parse. Nothing here can turn a CSS problem into a failed
// preview: the worst case is a sentence in the status line.

import { callExport, type WasmModule } from "../lib/wasm-call";

/** One problem, in the shape `tessera_css_diagnostics_json` writes. */
export interface CssDiagnostic {
  code: string;
  severity: "error" | "warning";
  message: string;
  file: string;
  /** 1-based, like every other diagnostic in this project. */
  line: number;
  column: number;
  offset: number;
}

/** The document that export returns. */
export interface CssDiagnosticsDocument {
  cssVersion: number;
  /** False when the module used the built-in fallback instead of the author's text. */
  styled: boolean;
  truncated: boolean;
  diagnostics: CssDiagnostic[];
}

/** The empty answer, for a module built before this export existed. */
const NONE: CssDiagnosticsDocument = Object.freeze({
  cssVersion: 2,
  styled: false,
  truncated: false,
  diagnostics: [],
});

/**
 * Asks `module` what is wrong with the stylesheets of the project it was staged with.
 *
 * Returns the empty document -- never throws -- when the module has no such export, because a page
 * published against an older artifact must keep previewing: it simply cannot apply the sheet, and the
 * caller's status line is where that is said. A module that *has* the export and throws is a real
 * failure and propagates, so a bug does not masquerade as "no problems".
 */
export async function readCssDiagnostics(module: WasmModule): Promise<CssDiagnosticsDocument> {
  if (typeof module._tessera_css_diagnostics_json !== "function") {
    return NONE;
  }
  const json = callExport(module, "_tessera_css_diagnostics_json");
  const document = JSON.parse(json) as CssDiagnosticsDocument;
  // A malformed document is not worth failing a preview over; the caller's contract is "a list,
  // possibly empty", and `diagnostics` is what it iterates.
  if (!Array.isArray(document.diagnostics)) {
    return { ...document, diagnostics: [] };
  }
  return document;
}

/** True when the sheet has at least one problem worth colouring the status line for. */
export function cssProblem(document: CssDiagnosticsDocument): boolean {
  return document.diagnostics.length > 0;
}

/**
 * A short suffix for the status line: ` · css 3 problem(s)`, ` · stylesheet not applied`, or nothing.
 *
 * The full text, with the line and column of each problem, is `describeCssDetails`'s job -- a status
 * line is one sentence, and a wall of rules in it would push the node count off the pane.
 */
export function describeCss(document: CssDiagnosticsDocument): string {
  const count = document.diagnostics.length;
  if (count === 0) {
    return "";
  }
  return ` · css ${count} problem(s)${document.truncated ? "+" : ""}`;
}

/**
 * One line per problem, `file:line:column: CODE: message`, which is the spelling the CLI prints and
 * what a person copies into a search. Bounded by the module's own cap, so this needs no limit.
 */
export function describeCssDetails(document: CssDiagnosticsDocument): string {
  return document.diagnostics
    .map((problem) => {
      const where = `${problem.file}:${problem.line}:${problem.column}`;
      return `${where}: ${problem.code}: ${problem.message}`;
    })
    .join("\n");
}
