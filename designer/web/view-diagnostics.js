// SPDX-License-Identifier: MIT

// The diagnostics view of the merged studio page (issue #392; the view itself is P1, issue #165).
//
// It compiles the shared source textarea with `tessera_diagnostics`, the same compiler the native CLI
// runs, and lists the diagnostics with their source spans (`diagnostics-view-render.js` renders them).
// This was `app.js` on its own page; the merge moved the page-level wiring (the toolbar, the gallery,
// the share link, the view tabs) into `studio-page.js` and `studio.js` and kept the compile path here
// unchanged, because that path is what the `wasm-parity` gate proves byte for byte.
//
// It never trusts that the module loaded: a missing or failed module leaves the view in an explicit
// "preview unavailable" state, because the Wasm artifacts are built by CI and a local checkout that has
// not built them must say so rather than show a blank pane. The module is imported on first use
// (`load()`), so the diagnostics bundle is the only one on the first-paint path -- and it is the one
// every visitor needs, because it is the default view.
//
// A diagnostics run has no viewport and no DPR: the compiler is handed a file name and nothing else.
// The toolbar's environment is *carried* by the share link on this view but not applied here, and the
// view says so rather than reporting a rectangle the compiler never used.

import { callExport, requireExports } from './wasm-call.js';
import { renderDiagnostics, unavailableBox } from './diagnostics-view-render.js';

const MODULE_URL = './tessera_diagnostics.js';

// A small sample that is valid, so the first Run shows the empty-diagnostics path. The gallery's
// samples are read from `samples/`; this stays as the zero-network default. It lives with the view
// because it is the text this view knows how to report on.
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

const FILE = 'ui/Preview.tsx';

/**
 * Mounts the diagnostics view.
 *
 * `options.source` is the page's one textarea (the merge's whole point: one editor for three views),
 * `options.setEnabled(name, enabled)` reports readiness to the page's shared Run button, and
 * `options.onSample` is the page's broadcast of a gallery pick.
 */
export function mountDiagnostics(options) {
  const source = options.source;
  const setEnabled = options.setEnabled || (() => {});
  const status = document.getElementById('diag-status');
  const list = document.getElementById('diagnostics');
  const emptyState = document.getElementById('diag-empty');

  // The module factory, or null when the artifacts are absent or refused to start.
  let factory = null;

  function setStatus(text, kind) {
    status.textContent = text;
    status.className = 'status' + (kind ? ' ' + kind : '');
  }

  function showUnavailable(detail) {
    list.replaceChildren();
    emptyState.hidden = true;
    list.append(
      unavailableBox(
        'Preview unavailable in this build',
        'The WebAssembly compiler did not load, so the diagnostics view cannot validate TSX. The ' +
          'module is built by CI and published with the page; a checkout that has not run the ' +
          '`wasm-compiler` build has no artifacts next to these files.',
        detail,
      ),
    );
    setStatus('module unavailable', 'warning');
  }

  function report(counts) {
    if (counts.total === 0) {
      setStatus('no diagnostics', 'ok');
      return;
    }
    const parts = [];
    if (counts.errors > 0) {
      parts.push(counts.errors + (counts.errors === 1 ? ' error' : ' errors'));
    }
    if (counts.warnings > 0) {
      parts.push(counts.warnings + (counts.warnings === 1 ? ' warning' : ' warnings'));
    }
    setStatus(parts.join(', '), counts.errors > 0 ? 'error' : 'warning');
  }

  async function run() {
    if (!factory) {
      return;
    }
    setEnabled('run', false);
    setStatus('compiling...');
    try {
      // One module instance per view: the compiler keeps no state between calls that the page needs,
      // and re-instantiating per Run would re-download and re-initialise the heap.
      const module = await factory();
      // The marshalling and the two release paths live in `wasm-call.js`: the merge gave three views
      // the same C ABI shape, so the lifetime rule is written once instead of three times.
      const json = callExport(module, '_tessera_diagnostics_json', [source.value, FILE]);
      report(renderDiagnostics(list, emptyState, JSON.parse(json), source));
    } catch (error) {
      list.replaceChildren();
      emptyState.hidden = true;
      list.append(unavailableBox('The compiler failed', String(error && error.message ? error.message : error)));
      setStatus('failed', 'error');
    } finally {
      setEnabled('run', true);
    }
  }

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
      // Instantiate once up front so a broken module fails visibly at load, not on the first Run, and
      // so a module built without the exports this view calls fails with a readable message rather
      // than as "x is not a function" later.
      requireExports(await factory(), ['_tessera_diagnostics_json']);
      setStatus('ready');
      setEnabled('run', true);
    } catch (error) {
      // A missing artifact is the normal state of a checkout that has not built Wasm; `import()` of a
      // missing file rejects with a TypeError whose message names the URL.
      factory = null;
      setEnabled('run', false);
      showUnavailable(error && error.message ? error.message : String(error));
    }
  }

  return {
    id: 'diagnostics',
    /** Loads the Wasm module; the page calls this when the view is first shown. */
    load: loadModule,
    /** The Run button's action (the keyboard shortcut lands here too). */
    run,
    /** "Load sample" on this view: the compiler's zero-network default into the shared editor. */
    loadSample: () => {
      source.value = SAMPLE;
      source.focus();
    },
    /** A gallery pick: the text is already in the textarea, so only the status line changes. */
    onSample: (sample) => setStatus(`loaded ${sample.path}`, 'ok'),
    /** This view has no layout, so an environment change is carried into the link and nothing more. */
    onEnvironmentChange: () => {},
    /** Only one view is on screen; the others never draw or re-layout, so this is a no-op here. */
    setVisible: () => {},
  };
}
