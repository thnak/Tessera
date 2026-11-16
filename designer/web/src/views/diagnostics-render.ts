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

/** Selects the span in the editor: there is no editor widget, so this is the whole "go to" affordance. */
function reveal(source: HTMLTextAreaElement, span: Span): void {
  source.focus();
  const start = Math.min(span.offset ?? 0, source.value.length);
  const length = Math.max(0, span.length ?? 0);
  source.setSelectionRange(start, Math.min(start + length, source.value.length));
  // Scroll the textarea so the caret is visible: a rough line-height estimate is enough and avoids
  // measuring the DOM for a diagnostics list.
  const lineHeight = parseFloat(getComputedStyle(source).lineHeight) || 20;
  source.scrollTop = Math.max(0, ((span.line ?? 1) - 3) * lineHeight);
}

function renderDiagnostic(diagnostic: Diagnostic, source: HTMLTextAreaElement): HTMLElement {
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

    const jump = element("button", null, "Show in source");
    jump.type = "button";
    jump.addEventListener("click", () => reveal(source, span));
    meta.append(jump);
    item.append(meta);

    const context = spotlight(source.value, span);
    if (context) {
      item.append(context);
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
 */
export function renderDiagnostics(
  list: HTMLElement,
  emptyState: HTMLElement,
  document_: DiagnosticsDocument,
  source: HTMLTextAreaElement,
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
