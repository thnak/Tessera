// SPDX-License-Identifier: MIT

// The scene view of the merged studio page (issue #392; the view is P2 issue #166 and P4 issue #168).
//
// It calls the same C exports the parity gates drive, parses the returned canonical documents, and
// renders the node list (path, kind, bounds, text) and the state cells after every scripted click
// (`scene-view-render.js` renders the nodes; `scene-input.js` renders the cells). This was `scene.js`
// on its own page; the merge moved the page chrome into `studio-page.js`/`shell.js`/`studio.js` and
// left the evaluation path here, including the second toggle that made this view special: the
// viewport and DPR are **layout inputs**, so a toolbar change re-runs the layout rather than waiting
// for the next Run.
//
// **What a click does here.** The click is dispatched inside the module by the engine's own input
// router, so hit testing, pointer capture, click synthesis and capture/target/bubble propagation are
// the production code paths. The handler it reaches is the handler's TSX *preview body*, executed by
// the IR evaluator -- the same body `tests/parity/parity_runner.cpp` runs. It is not the developer's
// C++: that code is not linked into this module and a click cannot run it. A handler that takes a
// payload parameter (`(e: ClickEvent) =>`) is reported as `unrun`, because the evaluator has no record
// value to bind to it. The view states this rather than implying real handler execution.
//
// The theme is carried in the link but not consumed by the null-RHI snapshot (the module has no theme
// asset), and that is stated rather than implied. The module is imported when this view is first
// shown, never on the first paint: a visitor who only wants diagnostics never downloads it.

import { parseViewport } from './preview-state.js';
import { mountInputScript } from './scene-input.js';
import { callExport, requireExports } from './wasm-call.js';
import { noSceneBox, renderNodes, unavailableBox } from './scene-view-render.js';

const MODULE_URL = './tessera_scene_preview.js';

// The preview's default layout rectangle (wasm_scene_entry.cpp's kDefaultWidth/Height/DpiScale): the
// plain `tessera_scene_json` export lays the tree out at exactly this, so the sized export is only
// needed when the toolbar has moved off it.
const DEFAULT_VIEWPORT = { width: 320, height: 240 };
const DEFAULT_DPR = 1;

// The parity-demo-shaped sample: a static tree the preview evaluates, so the first Run shows a real
// document instead of an unsupported-construct error. The button carries an explicit `automationId`
// so the input script below names a node that exists without the reader having to look up a hash.
export const SAMPLE = `export function Counter(): Node {
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

// The plain export is the parity gate's contract (byte-identical to the native CLI's 320x240 DPR 1)
// and is what a default run must keep calling; the sized one is for a moved toolbar. The fallback
// covers an artifact built before P5 landed next to this page.
function usesSizedExport(module, viewport, dpr) {
  const moved =
    viewport.width !== DEFAULT_VIEWPORT.width ||
    viewport.height !== DEFAULT_VIEWPORT.height ||
    dpr !== DEFAULT_DPR;
  return moved && typeof module._tessera_scene_json_sized === 'function';
}

/**
 * Mounts the scene view.
 *
 * `options.source` is the page's shared textarea, `options.studio()` returns the shared chrome (or
 * null before it mounts) so this view can read the toolbar and the current sample's file name, and
 * `options.setEnabled` reports readiness to the page's shared Run button.
 */
export function mountScene(options) {
  const source = options.source;
  const setEnabled = options.setEnabled || (() => {});
  const status = document.getElementById('scene-status');
  const list = document.getElementById('nodes');
  const emptyState = document.getElementById('scene-empty');
  const scriptInput = document.getElementById('script');
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

  function currentViewport() {
    const studio = options.studio();
    try {
      return parseViewport(studio ? studio.controls().viewport : '320x240');
    } catch {
      return { ...DEFAULT_VIEWPORT };
    }
  }

  function currentDpr() {
    const studio = options.studio();
    return Number(studio ? studio.controls().dpr : DEFAULT_DPR);
  }

  // The source path is an input to every `uiId` (docs/specs/automation-ids-v1.md section 2), so a
  // sample must be laid out under its own path or its ids would not match the native build's. The
  // studio owns it: it is the sample the visitor picked, or the default. The input script must use the
  // *same* path, or its `click <id>` steps would target ids from a different document.
  function currentFile() {
    const studio = options.studio();
    return studio ? studio.file() : 'ui/Preview.tsx';
  }

  /**
   * Appends the live viewport to the page's editor hint.
   *
   * The hint element is the page's (one shared editor), and `shell.js` sets the per-view sentence when
   * the tab changes. On this view the viewport is a real layout input, so the number in that sentence
   * has to follow the toolbar -- otherwise the hint would say "the toolbar re-runs the layout" next to
   * a rectangle it no longer names. Every other view leaves the static sentence alone.
   */
  function describeEnvironment() {
    const hint = document.getElementById('editor-hint');
    if (!hint) {
      return;
    }
    const viewport = currentViewport();
    const base = hint.textContent.split(' The viewport is ')[0];
    hint.textContent = `${base} The viewport is ${viewport.width}x${viewport.height} at DPR ${currentDpr()}.`;
  }

  async function run() {
    if (factory === null) {
      return;
    }
    setEnabled('run', false);
    setStatus('running', null);
    try {
      const module = await factory();
      const viewport = currentViewport();
      const dpr = currentDpr();
      const file = currentFile();
      const json = usesSizedExport(module, viewport, dpr)
        ? callExport(
            module,
            '_tessera_scene_json_sized',
            [source.value, file, ''],
            [viewport.width, viewport.height, dpr],
          )
        : callExport(module, '_tessera_scene_json', [source.value, file, '']);
      emptyState.hidden = true;
      const count = renderNodes(list, JSON.parse(json), (id) => input.append(id));
      if (count === 0) {
        list.replaceChildren();
        list.append(noSceneBox());
        setStatus('no scene', 'warning');
      } else {
        setStatus(`${count} node(s)`, 'ok');
      }
    } catch (error) {
      list.replaceChildren();
      emptyState.hidden = true;
      list.append(unavailableBox('Preview failed', String(error)));
      setStatus('failed', 'warning');
    } finally {
      setEnabled('run', true);
    }
  }

  // This half is `scene-input.js`: it owns the state-cell table and the document shape, and asks this
  // view for the document text so that the Wasm heap and `tessera_free` stay owned here. The "Send
  // clicks" button is the page's, grouped with the input pane it drives.
  const input = mountInputScript({
    source,
    script: scriptInput,
    button: options.buttons.runInput,
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

  async function loadModule() {
    if (factory !== null) {
      return;
    }
    try {
      const imported = await import(MODULE_URL);
      factory = imported.default;
      if (typeof factory !== 'function') {
        throw new Error('the module did not export a factory');
      }
      // Instantiate once so a module built without the exports this view calls fails at load with a
      // readable message, rather than as "x is not a function" on the first Run.
      requireExports(await factory(), ['_tessera_scene_json', '_tessera_input_json']);
      setEnabled('run', true);
      if (options.buttons.runInput) {
        options.buttons.runInput.disabled = false;
      }
      describeEnvironment();
      // The view is being opened for the first time with the source already in place: laying it out
      // straight away is what makes the Scene tab show a snapshot rather than an empty pane.
      await run();
    } catch (error) {
      list.replaceChildren();
      emptyState.hidden = true;
      list.append(
        unavailableBox(
          'Preview unavailable in this build',
          'The WebAssembly scene module did not load, so the scene view cannot lay the source out. ' +
            'The module is built by CI and published with the page; a checkout that has not run the ' +
            '`wasm-webgpu` build has no artifacts next to these files.',
          String(error),
        ),
      );
      setStatus('module unavailable', 'warning');
      setEnabled('run', false);
    }
  }

  function loadSample() {
    source.value = SAMPLE;
    scriptInput.value = SAMPLE_SCRIPT;
    input.reset();
    if (factory !== null) {
      void run();
    }
  }

  return {
    id: 'scene',
    /** Loads the Wasm module (and lays the current source out once) on the view's first open. */
    load: loadModule,
    /** The Run button's action, exposed so the page's shared Run button can drive it. */
    run,
    /** "Load sample" on this view: its own sample plus the three-click demo script. */
    loadSample,
    /** Only one view is on screen: a hidden scene pane neither draws nor re-lays out. */
    setVisible: () => {},
    /** Re-runs the layout because the toolbar changed: on this view the toggles are real inputs. */
    onEnvironmentChange: () => {
      describeEnvironment();
      if (factory !== null) {
        void run();
      }
    },
    /** A gallery pick: the previous sample's cells belong to a document this one did not evaluate. */
    onSample: (sample) => {
      setStatus(`loaded ${sample.path}`, 'ok');
      input.reset();
      scriptInput.value = '';
      describeEnvironment();
      if (factory !== null) {
        void run();
      }
    },
  };
}
