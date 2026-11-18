// SPDX-License-Identifier: MIT

// The diagnostics *list* of the Diagnostics view (the rendering is P1, issue #165; the module split is
// issue #392, the React host is issue #422).
//
// It takes a diagnostics document and the source text and returns DOM. Kept apart from the session
// (`diagnostics-session.ts`) for two reasons: it is the one part of that view that is pure rendering,
// and the session has to stay inside the source-size limit while carrying the Wasm lifecycle.
//
// The document shape is `docs/specs/compiler-diagnostics-v1.md`'s, produced by the same compiler the
// native CLI runs -- so this file must not invent, sort or filter anything. It shows the document.

import type { EditorSurface } from "../editor/code-editor";
import { element } from "./dom";

/** One diagnostic as the compiler reports it. */
export interface Diagnostic {
  severity?: string;
  code?: string;
  message?: string;
  suggestion?: string;
  span?: Span;
  related?: { message?: string; span?: Span }[];
}

export interface Span {
  file?: string;
  line?: number;
  column?: number;
  offset?: number;
  length?: number;
}

export interface DiagnosticsDocument {
  diagnostics?: Diagnostic[];
}

export interface DiagnosticCounts {
  total: number;
  errors: number;
  warnings: number;
}

/**
 * The source context of a span: the line the offset starts on, with the span underlined.
 *
 * The job is to make a span visible without an editor widget, so one line of context plus a wavy
 * underline is the whole feature.
 */
function spotlight(text: string, span: Span): HTMLElement | null {
  if (typeof span.offset !== "number" || span.offset < 0) {
    return null;
  }
  const start = Math.min(span.offset, text.length);
  const length = Math.max(0, typeof span.length === "number" ? span.length : 0);
  const end = Math.min(start + length, text.length);

  const lineStart = text.lastIndexOf("\n", Math.max(0, start - 1)) + 1;
  let lineEnd = text.indexOf("\n", start);
  if (lineEnd === -1) {
    lineEnd = text.length;
  }

  const box = element("p", "diagnostic-spotlight");
  box.append(document.createTextNode(text.slice(lineStart, start)));
  if (end > start) {
    box.append(element("span", "marked", text.slice(start, Math.min(end, lineEnd))));
    box.append(document.createTextNode(text.slice(Math.min(end, lineEnd), lineEnd)));
  } else {
    // A zero-length span (a stylesheet offset, for example) has nothing to underline; showing the
    // line is still the useful context.
    box.append(document.createTextNode(text.slice(start, lineEnd)));
  }
  return box;
}

/**
 * The source an open file would reveal, or `null` when the editor is showing a different document.
 *
 * The editor holds one project file at a time and a run diagnoses the whole project. "Show in source" can only mean "select this span in the
 * editor" when the editor is showing *this* text; otherwise the pane names the file and says where to
 * open it, rather than highlighting an offset in a document the diagnostic is not about.
 */
export interface RevealableSource {
  /** The editor, which holds the text of the file it is showing. */
  element: EditorSurface;
  /**
   * The project path the editor is showing. A run diagnoses a whole project, so each span is judged on its
   * own: it can be revealed only when its file is this one.
   */
  file: string;
}

/** Selects the span in the editor and scrolls it into view: the list's "go to" affordance. */
function reveal(element: EditorSurface, span: Span): void {
  element.reveal(span.offset ?? 0, span.length ?? 0);
}

function renderDiagnostic(
  diagnostic: Diagnostic,
  source: RevealableSource | null,
): HTMLElement {
  const severity = diagnostic.severity === "warning" ? "warning" : "error";
  const item = element("li", "diagnostic " + severity);

  const head = element("div", "diagnostic-head");
  head.append(
    element("span", "diagnostic-code", diagnostic.code || "(no code)"),
    element("span", "diagnostic-severity", severity),
  );
  item.append(head);

  item.append(element("p", "diagnostic-message", diagnostic.message || ""));

  const span = diagnostic.span;
  if (span) {
    const meta = element("div", "diagnostic-span");
    meta.append(element("span", null, `${span.file}:${span.line}:${span.column}`));

    if (source && span.file === source.file) {
      const jump = element("button", null, "Show in source");
      jump.type = "button";
      jump.addEventListener("click", () => {
        if (source.element) {
          reveal(source.element, span);
        }
      });
      meta.append(jump);
      item.append(meta);

      const context = spotlight(source.element.text(), span);
      if (context) {
        item.append(context);
      }
    } else {
      // The span is real but its text is not in the editor: name the file, so a reader can open the
      // tab rather than wonder why the button is gone.
      if (span.file) {
        meta.append(
          element(
            "span",
            "diagnostic-elsewhere",
            "open " + span.file + " to see the span",
          ),
        );
      }
      item.append(meta);
    }
  }

  if (diagnostic.suggestion) {
    item.append(element("p", "diagnostic-suggestion", diagnostic.suggestion));
  }
  for (const related of diagnostic.related ?? []) {
    const where = related.span
      ? `${related.span.file}:${related.span.line}:${related.span.column}: `
      : "";
    item.append(element("p", "diagnostic-related", where + (related.message || "")));
  }
  return item;
}

/**
 * Renders a diagnostics document into the list and returns the counts the status line reports.
 *
 * The caller owns the status line, because "no diagnostics" and "three warnings" are phrased
 * differently by a page that also has a scene view to talk about.
 *
 * `source` is the editor as a *revealable* document (issue #431): `null` means the page has no editor
 * for this text at all, and `shown: false` means the editor is holding another project file. Both show
 * the span's location and neither pretends the row leads anywhere it does not.
 */
export function renderDiagnostics(
  list: HTMLElement,
  emptyState: HTMLElement,
  document_: DiagnosticsDocument,
  source: RevealableSource | null,
): DiagnosticCounts {
  const diagnostics = Array.isArray(document_.diagnostics) ? document_.diagnostics : [];
  list.replaceChildren();
  emptyState.hidden = diagnostics.length > 0;

  for (const diagnostic of diagnostics) {
    list.append(renderDiagnostic(diagnostic, source));
  }

  const errors = diagnostics.filter((entry) => entry.severity !== "warning").length;
  return { total: diagnostics.length, errors, warnings: diagnostics.length - errors };
}
