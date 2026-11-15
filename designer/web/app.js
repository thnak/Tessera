// SPDX-License-Identifier: MIT

// The diagnostics page's logic (issue #165, P1). Vanilla ES modules: no framework, no bundler, no
// build step, so `index.html` + `app.js` + `app.css` + the two Wasm artifacts are the whole page.
//
// The page compiles in the browser with `tessera_diagnostics`, the same compiler the native CLI
// runs. It never trusts that the module loaded: a missing or failed module leaves the page in an
// explicit "preview unavailable" state, because the Wasm artifacts are built by CI and a local
// checkout that has not built them must say so rather than show a blank pane.

const MODULE_URL = './tessera_diagnostics.js';

// A small sample that is valid, so the first Run shows the empty-diagnostics path.
const SAMPLE = `export function Counter(): Node {
  const [count, setCount] = useState<int>(0);

  return (
    <View>
      <Text>Count: {count}</Text>
      <Button onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
`;

const source = document.getElementById('source');
const runButton = document.getElementById('run');
const sampleButton = document.getElementById('load-sample');
const status = document.getElementById('status');
const list = document.getElementById('diagnostics');
const emptyState = document.getElementById('empty-state');
const buildId = document.getElementById('build-id');

// The module factory, or null when the artifacts are absent or refused to start.
let factory = null;

function setStatus(text, kind) {
  status.textContent = text;
  status.className = 'status' + (kind ? ' ' + kind : '');
}

// Renders the "module did not load" state: the page is honest about what it cannot do instead of
// presenting an empty editor that silently never compiles.
function showUnavailable(detail) {
  list.replaceChildren();
  emptyState.hidden = true;
  const box = document.createElement('div');
  box.className = 'unavailable';
  const heading = document.createElement('h3');
  heading.textContent = 'Preview unavailable in this build';
  const body = document.createElement('p');
  body.textContent =
    'The WebAssembly compiler did not load, so this page cannot validate TSX. ' +
    'The module is built by CI and published with the page; a checkout that has not run the ' +
    '`wasm-compiler` build has no artifacts next to these files.';
  const reason = document.createElement('p');
  reason.textContent = detail ? 'Detail: ' + detail : '';
  box.append(heading, body);
  if (detail) {
    box.append(reason);
  }
  list.append(box);
  setStatus('module unavailable', 'warning');
}

// Escapes nothing: every string below goes through textContent, never innerHTML, so a diagnostic
// message that contains `<` or `&` cannot inject markup. That matters because messages quote source.
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

// The source context of a span: the line the offset starts on, with the span underlined. The
// job is to make a span visible without an editor widget (the gutter and highlighting are a later
// slice), so one line of context plus a wavy underline is the whole feature.
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
    // A zero-length span (a stylesheet offset, for example) has nothing to underline; showing the
    // line is still the useful context.
    box.append(document.createTextNode(text.slice(start, lineEnd)));
  }
  return box;
}

function renderDiagnostic(diagnostic) {
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
    const position = `${span.file}:${span.line}:${span.column}`;
    meta.append(element('span', '', position));

    const jump = element('button', '', 'Show in source');
    jump.type = 'button';
    jump.addEventListener('click', () => {
      // Selecting the span in the textarea is the whole "go to" affordance: no editor widget yet.
      source.focus();
      const start = Math.min(span.offset, source.value.length);
      const length = Math.max(0, span.length || 0);
      source.setSelectionRange(start, Math.min(start + length, source.value.length));
      // Scroll the textarea so the caret is visible: a rough line-height estimate is enough and
      // avoids measuring the DOM for a diagnostics-only page.
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

function render(document) {
  const diagnostics = Array.isArray(document.diagnostics) ? document.diagnostics : [];
  list.replaceChildren();
  emptyState.hidden = diagnostics.length > 0;

  for (const diagnostic of diagnostics) {
    list.append(renderDiagnostic(diagnostic));
  }

  const errors = diagnostics.filter((d) => d.severity !== 'warning').length;
  const warnings = diagnostics.length - errors;
  if (diagnostics.length === 0) {
    setStatus('no diagnostics', 'ok');
  } else {
    const parts = [];
    if (errors > 0) {
      parts.push(errors + (errors === 1 ? ' error' : ' errors'));
    }
    if (warnings > 0) {
      parts.push(warnings + (warnings === 1 ? ' warning' : ' warnings'));
    }
    setStatus(parts.join(', '), errors > 0 ? 'error' : 'warning');
  }
}

async function compile() {
  if (!factory) {
    return;
  }
  runButton.disabled = true;
  setStatus('compiling...');
  try {
    // One module instance per page: the compiler keeps no state between calls that the page needs,
    // and re-instantiating per Run would re-download and re-initialise the heap.
    const module = await factory();

    const text = source.value;
    const file = 'ui/Preview.tsx';

    // Allocate the two C strings in the module heap, call the export, read the result back, then
    // free all three. `tessera_free` is the module's matching release for the returned document;
    // `_free` releases the two inputs the page allocated itself.
    const length = module.lengthBytesUTF8(text) + 1;
    const textPtr = module._malloc(length);
    module.stringToUTF8(text, textPtr, length);
    const nameLength = module.lengthBytesUTF8(file) + 1;
    const namePtr = module._malloc(nameLength);
    module.stringToUTF8(file, namePtr, nameLength);

    let json = null;
    try {
      const resultPtr = module._tessera_diagnostics_json(textPtr, namePtr);
      if (!resultPtr) {
        throw new Error('the compiler returned no document (out of memory?)');
      }
      try {
        json = module.UTF8ToString(resultPtr);
      } finally {
        module._tessera_free(resultPtr);
      }
    } finally {
      module._free(textPtr);
      module._free(namePtr);
    }

    render(JSON.parse(json));
  } catch (error) {
    list.replaceChildren();
    emptyState.hidden = true;
    const box = element('div', 'unavailable');
    box.append(
      element('h3', '', 'The compiler failed'),
      element('p', '', String(error && error.message ? error.message : error)),
    );
    list.append(box);
    setStatus('failed', 'error');
  } finally {
    runButton.disabled = false;
  }
}

async function loadModule() {
  try {
    const imported = await import(MODULE_URL);
    factory = imported.default;
    if (typeof factory !== 'function') {
      throw new Error('the module did not export a factory');
    }
    // Instantiate once up front so a broken module fails visibly at load, not on the first Run.
    const probe = await factory();
    // The page calls these, so a module built without them (a changed EXPORTED_FUNCTIONS or
    // EXPORTED_RUNTIME_METHODS line) must fail here with a readable message rather than as a
    // "x is not a function" on the first Run.
    for (const name of ['_malloc', '_free', '_tessera_diagnostics_json', '_tessera_free',
                        'stringToUTF8', 'UTF8ToString', 'lengthBytesUTF8']) {
      if (typeof probe[name] !== 'function') {
        throw new Error('the module is missing `' + name + '`');
      }
    }
    setStatus('ready');
    runButton.disabled = false;
  } catch (error) {
    // A missing artifact is the normal state of a checkout that has not built Wasm; `import()` of a
    // missing file rejects with a TypeError whose message names the URL.
    factory = null;
    runButton.disabled = true;
    showUnavailable(error && error.message ? error.message : String(error));
  }
}

runButton.addEventListener('click', compile);
sampleButton.addEventListener('click', () => {
  source.value = SAMPLE;
  source.focus();
});
source.addEventListener('keydown', (event) => {
  if ((event.ctrlKey || event.metaKey) && event.key === 'Enter') {
    event.preventDefault();
    compile();
  }
});

// The build id the page reports: the footer of the deployed artifact carries it, and D-S5's
// acceptance is that the deployed URL serves the current build. `build-id.txt` is written by the
// publish job; when it is absent (a local checkout) the field says so rather than inventing one.
fetch('./build-id.txt')
  .then((response) => (response.ok ? response.text() : Promise.reject(new Error('absent'))))
  .then((text) => {
    const id = text.trim();
    buildId.textContent = 'build: ' + (id || 'unknown');
  })
  .catch(() => {
    buildId.textContent = 'build: local (no build-id.txt)';
    buildId.title = 'The publish job writes build-id.txt; a local checkout has none.';
  });

source.value = SAMPLE;
runButton.disabled = true;
loadModule();
