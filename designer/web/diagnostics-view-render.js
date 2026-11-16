// SPDX-License-Identifier: MIT

// The diagnostics *list* of the merged studio page's Diagnostics view (issue #392; the rendering
// itself is P1, issue #165). Split out of `view-diagnostics.js` so neither file passes the repository's
// source-size limit, and because it is the one part of that view that is pure rendering: it takes a
// diagnostics document and a textarea and returns DOM.
//
// Every string goes through `textContent`, never `innerHTML`, so a diagnostic message that quotes
// source (`<`, `&`, a stray tag) cannot inject markup. That matters more than usual here: the text
// being quoted is user input.

// Escapes nothing: `textContent` is the whole escaping story.
function element(tag, className, text) {
  const node = document.createElement(tag);
  if (className) {
    node.className = className;
  }
  if (text !== undefined) {
    node.textContent = text;
  }
  return node;
}

// The source context of a span: the line the offset starts on, with the span underlined. The job is to
// make a span visible without an editor widget, so one line of context plus a wavy underline is the
// whole feature.
function spotlight(text, span) {
  if (!span || typeof span.offset !== 'number' || span.offset < 0) {
    return null;
  }
  const start = Math.min(span.offset, text.length);
  const length = Math.max(0, typeof span.length === 'number' ? span.length : 0);
  const end = Math.min(start + length, text.length);

  const lineStart = text.lastIndexOf('\n', Math.max(0, start - 1)) + 1;
  let lineEnd = text.indexOf('\n', start);
  if (lineEnd === -1) {
    lineEnd = text.length;
  }

  const box = element('p', 'diagnostic-spotlight');
  box.append(document.createTextNode(text.slice(lineStart, start)));
  if (end > start) {
    box.append(element('span', 'marked', text.slice(start, Math.min(end, lineEnd))));
    box.append(document.createTextNode(text.slice(Math.min(end, lineEnd), lineEnd)));
  } else {
    // A zero-length span (a stylesheet offset, for example) has nothing to underline; showing the line
    // is still the useful context.
    box.append(document.createTextNode(text.slice(start, lineEnd)));
  }
  return box;
}

function renderDiagnostic(diagnostic, source) {
  const severity = diagnostic.severity === 'warning' ? 'warning' : 'error';
  const item = element('li', 'diagnostic ' + severity);

  const head = element('div', 'diagnostic-head');
  head.append(
    element('span', 'diagnostic-code', diagnostic.code || '(no code)'),
    element('span', 'diagnostic-severity', severity),
  );
  item.append(head);

  item.append(element('p', 'diagnostic-message', diagnostic.message || ''));

  const span = diagnostic.span;
  if (span) {
    const meta = element('div', 'diagnostic-span');
    meta.append(element('span', '', `${span.file}:${span.line}:${span.column}`));

    const jump = element('button', '', 'Show in source');
    jump.type = 'button';
    jump.addEventListener('click', () => {
      // Selecting the span in the textarea is the whole "go to" affordance: there is no editor widget.
      source.focus();
      const start = Math.min(span.offset, source.value.length);
      const length = Math.max(0, span.length || 0);
      source.setSelectionRange(start, Math.min(start + length, source.value.length));
      // Scroll the textarea so the caret is visible: a rough line-height estimate is enough and avoids
      // measuring the DOM for a diagnostics list.
      const lineHeight = parseFloat(getComputedStyle(source).lineHeight) || 20;
      source.scrollTop = Math.max(0, (span.line - 3) * lineHeight);
    });
    meta.append(jump);
    item.append(meta);

    const context = spotlight(source.value, span);
    if (context) {
      item.append(context);
    }
  }

  if (diagnostic.suggestion) {
    item.append(element('p', 'diagnostic-suggestion', diagnostic.suggestion));
  }
  if (Array.isArray(diagnostic.related)) {
    for (const related of diagnostic.related) {
      const where = related.span
        ? `${related.span.file}:${related.span.line}:${related.span.column}: `
        : '';
      item.append(element('p', 'diagnostic-related', where + (related.message || '')));
    }
  }
  return item;
}

/**
 * Renders a diagnostics document into the list, and returns the counts the status line reports.
 *
 * The caller owns the status line, because "no diagnostics" and "three warnings" are phrased
 * differently by a page that also has a scene view to talk about.
 */
export function renderDiagnostics(list, emptyState, document_, source) {
  const diagnostics = Array.isArray(document_.diagnostics) ? document_.diagnostics : [];
  list.replaceChildren();
  emptyState.hidden = diagnostics.length > 0;

  for (const diagnostic of diagnostics) {
    list.append(renderDiagnostic(diagnostic, source));
  }

  const errors = diagnostics.filter((d) => d.severity !== 'warning').length;
  return { total: diagnostics.length, errors, warnings: diagnostics.length - errors };
}

/** The "module did not load" or "the compiler failed" box: one shape for both. */
export function unavailableBox(heading, body, detail) {
  const box = element('div', 'unavailable');
  box.append(element('h3', '', heading), element('p', '', body));
  if (detail) {
    box.append(element('p', '', 'Detail: ' + detail));
  }
  return box;
}
