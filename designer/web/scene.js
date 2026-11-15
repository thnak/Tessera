// SPDX-License-Identifier: MIT

// The scene-snapshot debug page (issue #166, P2; input and state, issue #168, P4 of the design-time
// preview). Vanilla ES modules: no framework, no bundler, no build step, so `scene.html` + `scene.js`
// + `app.css` + the two Wasm artifacts are the whole page.
//
// It calls the same C exports the parity gates drive, parses the returned canonical documents, and
// renders the node list (path, kind, bounds, text) and the state cells after every scripted click.
//
// **What a click does here.** The click is dispatched inside the module by the engine's own input
// router, so hit testing, pointer capture, click synthesis and capture/target/bubble propagation are
// the production code paths. The handler it reaches is the handler's TSX *preview body*, executed by
// the IR evaluator — the same body `tests/parity/parity_runner.cpp` runs. It is not the developer's
// C++: that code is not linked into this module and a click cannot run it. A handler that takes a
// payload parameter (`(e: ClickEvent) =>`) is reported as `unrun`, because the evaluator has no
// record value to bind to it. The page states this rather than implying real handler execution.
//
// The page never trusts that the module loaded: a missing artifact leaves it in an explicit
// "preview unavailable" state, because the module is built by CI and a local checkout that has not
// built it must say so rather than show an empty pane (the same rule as the P1 diagnostics page).

const MODULE_URL = './tessera_scene_preview.js';

// The parity-demo-shaped sample: a static tree the preview evaluates, so the first Run shows a real
// document instead of an unsupported-construct error. The button carries an explicit `automationId`
// so the input script below names a node that exists without the reader having to look up a hash.
const SAMPLE = `export function Counter(): Node {
  const [count, setCount] = useState<int>(0);

  return (
    <View>
      <Text>Count: {count}</Text>
      <Button automationId="101" onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
`;

const SAMPLE_SCRIPT = 'click 101\nclick 101\nclick 101\n';

const source = document.getElementById('source');
const runButton = document.getElementById('run');
const sampleButton = document.getElementById('load-sample');
const status = document.getElementById('status');
const list = document.getElementById('nodes');
const emptyState = document.getElementById('empty-state');
const buildId = document.getElementById('build-id');
const scriptInput = document.getElementById('script');
const runInputButton = document.getElementById('run-input');
const inputStatus = document.getElementById('input-status');
const cells = document.getElementById('cells');

// The module factory, or null when the artifacts are absent or refused to start.
let factory = null;

function setStatus(text, kind) {
  status.textContent = text;
  status.className = 'status' + (kind ? ' ' + kind : '');
}

function setInputStatus(text, kind) {
  inputStatus.textContent = text;
  inputStatus.className = 'status' + (kind ? ' ' + kind : '');
}

// Renders the "module did not load" state instead of presenting an editor that silently never
// evaluates.
function showUnavailable(detail) {
  list.replaceChildren();
  emptyState.hidden = true;
  const box = document.createElement('div');
  box.className = 'unavailable';
  const heading = document.createElement('h3');
  heading.textContent = 'Preview unavailable in this build';
  const body = document.createElement('p');
  body.textContent =
    'The WebAssembly scene module did not load, so this page cannot lay the source out. ' +
    'The module is built by CI and published with the page; a checkout that has not run the ' +
    '`wasm-webgpu` build has no artifacts next to these files.';
  box.append(heading, body);
  if (detail) {
    const reason = document.createElement('p');
    reason.textContent = 'Detail: ' + detail;
    box.append(reason);
  }
  list.append(box);
  setStatus('module unavailable', 'warning');
}

// The C ABI is bytes in the module's heap, and the module owns the document it returns:
// `tessera_free` is the documented matching release, so every allocation below is undone. Both
// exports take four UTF-8 strings, so the marshalling lives here once instead of at each call site.
function callExport(module, name, args) {
  const pointers = [];
  try {
    for (const text of args) {
      const length = module.lengthBytesUTF8(text) + 1;
      const pointer = module._malloc(length);
      module.stringToUTF8(text, pointer, length);
      pointers.push(pointer);
    }
    const resultPtr = module[name](...pointers);
    if (!resultPtr) {
      throw new Error(name + ' returned null');
    }
    try {
      return module.UTF8ToString(resultPtr);
    } finally {
      module._tessera_free(resultPtr);
    }
  } finally {
    for (const pointer of pointers) {
      module._free(pointer);
    }
  }
}

// Every string below goes through textContent, never innerHTML, so a text payload containing `<` or
// `&` cannot inject markup.
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

async function loadModule() {
  try {
    const imported = await import(MODULE_URL);
    factory = imported.default;
    runButton.disabled = false;
    runInputButton.disabled = false;
  } catch (error) {
    showUnavailable(String(error));
  }
}

// The bounds are fixed three-decimal numbers in the document; this only makes them readable.
function describeBounds(bounds) {
  if (!bounds || typeof bounds !== 'object') {
    return '';
  }
  const number = (value) => (typeof value === 'number' ? value.toFixed(0) : '?');
  return `${number(bounds.x)},${number(bounds.y)} ${number(bounds.width)}x${number(
    bounds.height
  )}`;
}

function render(document) {
  list.replaceChildren();
  emptyState.hidden = true;
  const nodes = Array.isArray(document.nodes) ? document.nodes : [];
  if (nodes.length === 0) {
    // An empty document is the documented answer for a source the preview does not evaluate (an
    // unsupported construct, or a diagnostic). It is shown as such, not as a successful empty page.
    const note = element('div', 'unavailable');
    note.append(
      element('h3', null, 'No scene'),
      element(
        'p',
        null,
        'The source produced no scene. The preview evaluates the static subset; a user component ' +
          'instance, a conditional or repeated child, or an inline-span Text is reported as ' +
          'unsupported (docs/design/compiler-wasm.md section 7.1). Run the P1 diagnostics page ' +
          'for the compile error, if there is one.'
      )
    );
    list.append(note);
    setStatus('no scene', 'warning');
    return;
  }
  for (const node of nodes) {
    const row = element('li', 'node');
    row.style.paddingLeft = `${8 + (Number(node.depth) || 0) * 18}px`;
    row.append(
      element('span', 'node-kind', String(node.kind ?? '')),
      element('span', 'node-path', String(node.path ?? '')),
      element('span', 'node-bounds', describeBounds(node.bounds))
    );
    if (node.text) {
      row.append(element('span', 'node-text', String(node.text)));
    }
    if (node.uiId && node.uiId !== '0') {
      row.append(element('span', 'node-id', `uiId ${node.uiId}`));
      // The row is the id picker: a click appends `click <id>` to the script, so a reader never has
      // to copy a 64-bit number out of the list.
      const pick = element('button', 'node-pick', 'click');
      pick.type = 'button';
      pick.title = `Append \`click ${node.uiId}\` to the input script`;
      pick.addEventListener('click', () => {
        appendClick(String(node.uiId));
      });
      row.append(pick);
    }
    list.append(row);
  }
  setStatus(`${nodes.length} node(s)`, 'ok');
}

async function run() {
  if (factory === null) {
    return;
  }
  runButton.disabled = true;
  setStatus('running', null);
  let module = null;
  try {
    module = await factory();
    const json = callExport(module, '_tessera_scene_json', [source.value, 'ui/Preview.tsx', '']);
    render(JSON.parse(json));
  } catch (error) {
    list.replaceChildren();
    emptyState.hidden = true;
    const box = element('div', 'unavailable');
    box.append(
      element('h3', null, 'Preview failed'),
      element('p', null, String(error))
    );
    list.append(box);
    setStatus('failed', 'warning');
  } finally {
    runButton.disabled = false;
  }
}

// One state cell as a readable row: the cell id, then its value. A list cell keeps its brackets so a
// snapshot read by eye matches the document's own shape.
function describeCell(cell) {
  const value = cell.value;
  if (value === undefined || value === null) {
    return '(no value)';
  }
  if (Array.isArray(value)) {
    return '[' + value.map((item) => String(item.value ?? '')).join(', ') + ']';
  }
  return String(value.value ?? '');
}

function renderCells(document) {
  cells.replaceChildren();
  const initial = Array.isArray(document.initial) ? document.initial : [];
  const steps = Array.isArray(document.steps) ? document.steps : [];
  if (initial.length === 0 && steps.length === 0) {
    setInputStatus('no state cells', 'warning');
    return;
  }
  // The initial state first, then every step's cells, so the sequence is visible rather than only the
  // final value: a preview that dropped intermediate state still ends at the same place in a simple
  // counter, and this is what makes that difference readable.
  const addRow = (label, state, extra) => {
    const row = element('li', extra ? 'cell step' : 'cell');
    row.append(element('span', 'cell-when', label));
    if (state.length === 0) {
      row.append(element('span', 'cell-name', '(no cells)'));
    }
    for (const cell of state) {
      const pair = element('span', 'cell-pair');
      pair.append(
        element('span', 'cell-name', String(cell.name ?? '')),
        element('span', 'cell-value', describeCell(cell))
      );
      row.append(pair);
    }
    cells.append(row);
  };
  addRow('initial', initial, false);
  for (const step of steps) {
    const label = `click ${step.automationId}`;
    const note = step.found
      ? step.dispatched
        ? ''
        : ' (not dispatched)'
      : ' (no such node)';
    addRow(label + note, Array.isArray(step.state) ? step.state : [], true);
  }
  const last = steps.length > 0 ? steps[steps.length - 1] : null;
  if (last === null) {
    setInputStatus(`${initial.length} cell(s) at their initial value`, 'ok');
    return;
  }
  const ran = (last.ran || []).length;
  const unrun = (last.unrun || []).length;
  const parts = [`${steps.length} step(s)`, `${ran} handler run`];
  if (unrun > 0) {
    parts.push(`${unrun} handler not run`);
  }
  setInputStatus(parts.join(', '), unrun > 0 ? 'warning' : 'ok');
}

async function runInput() {
  if (factory === null) {
    return;
  }
  runInputButton.disabled = true;
  setInputStatus('clicking', null);
  let module = null;
  try {
    module = await factory();
    const json = callExport(module, '_tessera_input_json', [
      source.value,
      'ui/Preview.tsx',
      '',
      scriptInput.value,
    ]);
    const document = JSON.parse(json);
    if (document.error) {
      // A malformed script, or a source that produced no scene: reported as such, never as a
      // successful run with no clicks.
      cells.replaceChildren();
      const box = element('div', 'unavailable');
      box.append(element('h3', null, 'No input result'), element('p', null, String(document.error)));
      cells.append(box);
      setInputStatus('failed', 'warning');
      return;
    }
    renderCells(document);
  } catch (error) {
    cells.replaceChildren();
    const box = element('div', 'unavailable');
    box.append(element('h3', null, 'Input failed'), element('p', null, String(error)));
    cells.append(box);
    setInputStatus('failed', 'warning');
  } finally {
    runInputButton.disabled = false;
  }
}

// Appending a node's id to the script is what makes the page usable without a hash table in the
// reader's head: the ids are 64-bit numbers, so nobody types them.
function appendClick(automationId) {
  if (!automationId || automationId === '0') {
    return;
  }
  const line = `click ${automationId}`;
  const current = scriptInput.value.trim();
  scriptInput.value = current ? `${current}\n${line}\n` : `${line}\n`;
  setInputStatus(`added ${line}`, 'ok');
}

runButton.addEventListener('click', () => {
  void run();
});
sampleButton.addEventListener('click', () => {
  source.value = SAMPLE;
  scriptInput.value = SAMPLE_SCRIPT;
});
runInputButton.addEventListener('click', () => {
  void runInput();
});
source.addEventListener('keydown', (event) => {
  if ((event.ctrlKey || event.metaKey) && event.key === 'Enter') {
    event.preventDefault();
    void run();
  }
});

// `build-id.txt` is written by the publish job; a local checkout has none, and the field says so
// rather than inventing one.
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
scriptInput.value = SAMPLE_SCRIPT;
runButton.disabled = true;
runInputButton.disabled = true;
void loadModule();
