// SPDX-License-Identifier: MIT

// The scene-snapshot debug page (issue #166, P2 of the design-time preview). Vanilla ES modules: no
// framework, no bundler, no build step, so `scene.html` + `scene.js` + `app.css` + the two Wasm
// artifacts are the whole page.
//
// It is deliberately a *debug* page, not the P2 tree UI: it calls the same C export the parity gate
// drives, parses the returned canonical document, and renders the node list with each node's path,
// kind, bounds and text. That is enough to see the evaluator, the scene and the layout working in a
// browser without WebGPU, which is what this slice set out to prove; picking, the styled tree panel
// and the canvas are P3 onwards (docs/design/reference-equantic-ui.md section 7.4).
//
// The page never trusts that the module loaded: a missing artifact leaves it in an explicit
// "preview unavailable" state, because the module is built by CI and a local checkout that has not
// built it must say so rather than show an empty pane (the same rule as the P1 diagnostics page).

const MODULE_URL = './tessera_scene_preview.js';

// The parity-demo-shaped sample: a static tree the preview evaluates, so the first Run shows a real
// document instead of an unsupported-construct error.
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
const list = document.getElementById('nodes');
const emptyState = document.getElementById('empty-state');
const buildId = document.getElementById('build-id');

// The module factory, or null when the artifacts are absent or refused to start.
let factory = null;

function setStatus(text, kind) {
  status.textContent = text;
  status.className = 'status' + (kind ? ' ' + kind : '');
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
    const text = source.value;
    const file = 'ui/Preview.tsx';

    // The C ABI is bytes in the module's heap, and the module owns the document it returns:
    // `tessera_free` is the documented matching release, so every allocation below is undone.
    const textLength = module.lengthBytesUTF8(text) + 1;
    const textPtr = module._malloc(textLength);
    const nameLength = module.lengthBytesUTF8(file) + 1;
    const namePtr = module._malloc(nameLength);
    const componentLength = 1;
    const componentPtr = module._malloc(componentLength);
    let json;
    try {
      module.stringToUTF8(text, textPtr, textLength);
      module.stringToUTF8(file, namePtr, nameLength);
      module.stringToUTF8('', componentPtr, componentLength);
      const resultPtr = module._tessera_scene_json(textPtr, namePtr, componentPtr);
      if (!resultPtr) {
        throw new Error('tessera_scene_json returned null');
      }
      try {
        json = module.UTF8ToString(resultPtr);
      } finally {
        module._tessera_free(resultPtr);
      }
    } finally {
      module._free(textPtr);
      module._free(namePtr);
      module._free(componentPtr);
    }
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

runButton.addEventListener('click', () => {
  void run();
});
sampleButton.addEventListener('click', () => {
  source.value = SAMPLE;
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
runButton.disabled = true;
void loadModule();
