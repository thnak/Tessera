// SPDX-License-Identifier: MIT

// The scene-snapshot debug page (issue #166, P2; input and state, issue #168, P4; the share link,
// gallery and viewport/DPR/theme toolbar, issue #169, P5, of the design-time preview). Vanilla ES
// modules: no framework, no bundler, no build step, so `scene.html` + `scene.js` + `scene-input.js` +
// `studio.js` + `app.css` + the two Wasm artifacts are the whole page.
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
// On this page the P5 viewport and DPR toggles are **real**: `tessera_scene_json_sized` lays the tree
// out at the chosen rectangle and `dpi_scale`, so a shared link reproduces the same bounds. The theme
// is carried in the link but not consumed by the null-RHI snapshot (the module has no theme asset), and
// that is stated rather than implied.
//
// The page never trusts that the module loaded: a missing artifact leaves it in an explicit
// "preview unavailable" state, because the module is built by CI and a local checkout that has not
// built it must say so rather than show an empty pane (the same rule as the P1 diagnostics page).

import { mountStudio } from './studio.js';
import { parseViewport } from './preview-state.js';
import { element, mountInputScript } from './scene-input.js';

const MODULE_URL = './tessera_scene_preview.js';

// The preview's default layout rectangle (wasm_scene_entry.cpp's kDefaultWidth/Height/DpiScale): the
// plain `tessera_scene_json` export lays the tree out at exactly this, so the sized export is only
// needed when the toolbar has moved off it.
const DEFAULT_VIEWPORT = { width: 320, height: 240 };
const DEFAULT_DPR = 1;

// The parity-demo-shaped sample: a static tree the preview evaluates, so the first Run shows a real
// document instead of an unsupported-construct error. The button carries an explicit `automationId`
// so the input script below names a node that exists without the reader having to look up a hash. The
// gallery's samples come from `samples/`.
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
// The *first* `.hint` is the source pane's (the input pane has its own, static text), and it is where
// the P5 toolbar writes the viewport it last applied.
const hint = document.querySelector('.hint');

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
// `tessera_free` is the documented matching release, so every allocation below is undone — the
// argument strings and the returned block, on the success path and on a throw. Every export takes
// UTF-8 strings followed by numbers (the sized export adds width, height and DPR), so the marshalling
// lives here once instead of at each call site.
function callExport(module, name, args, numbers = []) {
  const pointers = [];
  try {
    for (const text of args) {
      const length = module.lengthBytesUTF8(text) + 1;
      const pointer = module._malloc(length);
      module.stringToUTF8(text, pointer, length);
      pointers.push(pointer);
    }
    const resultPtr = module[name](...pointers, ...numbers);
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
        input.append(String(node.uiId));
      });
      row.append(pick);
    }
    list.append(row);
  }
  setStatus(`${nodes.length} node(s)`, 'ok');
}

// ---- P5: environment toolbar, sample gallery, share link (issue #169) --------------------------

// The page's P5 chrome is one call, shared with the diagnostics page (`studio.js`). Unlike that page,
// this one *applies* the environment: the viewport and DPR are layout inputs, so a change re-runs
// immediately rather than waiting for the next Run -- a toggle that does nothing until you press Run
// reads as broken.
let studio = null;

function currentViewport() {
  return parseViewport(studio ? studio.controls().viewport : '320x240');
}

function currentDpr() {
  return Number(studio ? studio.controls().dpr : DEFAULT_DPR);
}

// The source path is an input to every `uiId` (docs/specs/automation-ids-v1.md section 2), so a sample
// must be laid out under its own path or its ids would not match the native build's. The studio owns
// it: it is the sample the visitor picked, or the default. The input script must use the *same* path,
// or its `click <id>` steps would target ids from a different document.
function currentFile() {
  return studio ? studio.file() : 'ui/Preview.tsx';
}

function usesSizedExport(module, viewport, dpr) {
  // The plain export is the parity gate's contract (byte-identical to the native CLI's 320x240 DPR 1)
  // and is what a default run must keep calling; the sized one is for a moved toolbar. The fallback
  // covers an artifact built before P5 landed next to this page.
  const moved = viewport.width !== DEFAULT_VIEWPORT.width
    || viewport.height !== DEFAULT_VIEWPORT.height
    || dpr !== DEFAULT_DPR;
  return moved && typeof module._tessera_scene_json_sized === 'function';
}

async function run() {
  if (factory === null) {
    return;
  }
  runButton.disabled = true;
  setStatus('running', null);
  try {
    const module = await factory();
    const viewport = currentViewport();
    const dpr = currentDpr();
    const text = source.value;
    const file = currentFile();
    const json = usesSizedExport(module, viewport, dpr)
      ? callExport(
          module,
          '_tessera_scene_json_sized',
          [text, file, ''],
          [viewport.width, viewport.height, dpr]
        )
      : callExport(module, '_tessera_scene_json', [text, file, '']);
    render(JSON.parse(json));
  } catch (error) {
    list.replaceChildren();
    emptyState.hidden = true;
    const box = element('div', 'unavailable');
    box.append(element('h3', null, 'Preview failed'), element('p', null, String(error)));
    list.append(box);
    setStatus('failed', 'warning');
  } finally {
    runButton.disabled = false;
  }
}

// ---- P4: the input script and the state cells (issue #168) -------------------------------------

// This half is `scene-input.js`: it owns the textarea, the state-cell table and the document shape,
// and asks this page for the document text so that the Wasm heap and `tessera_free` stay owned here.
const input = mountInputScript({
  source,
  script: scriptInput,
  button: runInputButton,
  cells,
  file: currentFile,
  request: async (name, args) => {
    if (factory === null) {
      throw new Error('the WebAssembly module is not loaded');
    }
    return callExport(await factory(), name, args);
  },
  setStatus: setInputStatus,
});

// ---- wiring ------------------------------------------------------------------------------------

let mountedStudio = mountStudio({
  source,
  startup: SAMPLE,
  onEnvironmentChange: (next) => {
    if (hint) {
      hint.textContent = `Ctrl+Enter (or Cmd+Enter) runs. The viewport is ${next.viewport} at DPR ${next.dpr}.`;
    }
    if (factory !== null) {
      void run();
    }
  },
  onSample: (sample) => {
    setStatus(`loaded ${sample.path}`, 'ok');
    // The previous sample's cells belong to a document this one did not evaluate, and its script may
    // name ids it does not have; the pane says so rather than showing another sample's state.
    input.reset();
    scriptInput.value = '';
    if (factory !== null) {
      void run();
    }
  },
});
mountedStudio
  .then((mounted) => {
    studio = mounted;
    if (hint) {
      const viewport = currentViewport();
      hint.textContent = `Ctrl+Enter (or Cmd+Enter) runs. The viewport is ${viewport.width}x${viewport.height} at DPR ${currentDpr()}.`;
    }
  })
  .catch((error) => {
    // A mount failure is the shared chrome being absent (an old `scene.html` next to a new script),
    // and the page must say so instead of throwing where nothing renders the error.
    showUnavailable(String(error));
  });

runButton.addEventListener('click', () => {
  void run();
});
sampleButton.addEventListener('click', () => {
  source.value = SAMPLE;
  scriptInput.value = SAMPLE_SCRIPT;
  input.reset();
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
