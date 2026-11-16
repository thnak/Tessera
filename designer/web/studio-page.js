// SPDX-License-Identifier: MIT

// The merged studio page's entry point (issue #392: the merge of the P1 diagnostics page, the P2/P4
// scene page and the P3 canvas page into one `index.html`).
//
// **One page, three views, one set of controls.** `index.html` carries one header, one toolbar
// (viewport, DPR, theme), one sample gallery, one share button, one source editor and one Run, and
// three panes. This module decides which pane is showing, mounts each pane on first use, and routes
// the shared controls -- Run, Load sample, the environment toolbar, a gallery pick, a share link -- to
// whichever view is actually on screen. Before the merge each of the three pages had its own copy of
// that wiring, and the copies had already drifted: all three carried a `build-id.txt` fetch, and the
// canvas page carried no toolbar at all.
//
// **Laziness, which is the merge's central constraint.** Only the diagnostics view loads at first
// paint, and only its module is fetched then. The scene view's module is fetched when the Scene tab is
// first opened; the canvas view's module *and* its WebGPU device are fetched when Canvas is first
// opened, and the device is not acquired until the first Run on that view. So `cold_engine_ready_ms`
// and the first-paint request set do not grow: a visitor who only reads diagnostics pays for exactly
// what the P1 page cost, and a browser without WebGPU keeps Diagnostics and Scene and sees the named
// unsupported state in Canvas only (docs/design/reference-equantic-ui.md section 7.5).
//
// **Three modules can coexist.** The merge does not merge the Wasm builds: `tessera_diagnostics`,
// `tessera_scene_preview` and `tessera_canvas` stay three modules with three `.wasm` files, and each
// view imports only its own. A failed WebGPU import therefore cannot take the snapshot view down.
//
// **Nothing leaves the browser.** No network call beyond the page's own static assets (its module
// files, `samples.json`, a sample's TSX, `build-id.txt`), no CDN, no telemetry. The share link is a URL
// fragment, decoded locally.

import { mountShell, resolveView, viewportOf, DEFAULT_VIEW } from './shell.js';
import { mountStudio } from './studio.js';
import { DEFAULT_FILE } from './preview-state.js';

// The views, by id. Each entry is a dynamic import, so the module a view needs is fetched the first
// time the view is opened and never before -- this object holds functions, not modules.
const VIEW_LOADERS = {
  diagnostics: () => import('./view-diagnostics.js'),
  scene: () => import('./view-scene.js'),
  canvas: () => import('./view-canvas.js'),
};

/**
 * The sample the page starts with.
 *
 * This is the **diagnostics** view's sample, not the scene view's, because Diagnostics is the default
 * view and the merged page compiles the editor on the first Run. The scene view's own sample carries
 * an explicit `automationId`, which the compiler *rejects* unless it equals the computed `uiId`
 * (`TESSERA_IR_AUTOMATION_ID`) -- so a page whose startup sample had one would show a compile error
 * before the visitor typed anything. The scene view's "Load sample" button loads its own sample plus
 * its three-click script, exactly as the pre-merge scene page did; the shared editor means the two
 * samples have to stay distinct rather than one sample pretending to be both.
 */
function startupSample() {
  return `export function Counter(): Node {
  const [count, setCount] = useState<int>(0);

  return (
    <View>
      <Text>Count: {count}</Text>
      <Button onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
`;
}

const source = document.getElementById('source');
const runButton = document.getElementById('run');
const sampleButton = document.getElementById('load-sample');
const buildId = document.getElementById('build-id');

// The mounted view instances, by id. A view is mounted once, the first time its tab is opened.
const views = new Map();
// The shared chrome (toolbar, gallery, share link), once `mountStudio` resolves.
let studio = null;
// The view switcher, once `mountShell` has run.
let shell = null;

/** Enables/disables one of the page's shared buttons on behalf of the view that is showing. */
function setEnabled(name, enabled) {
  const button = name === 'run' ? runButton : name === 'loadSample' ? sampleButton : null;
  if (button) {
    button.disabled = !enabled;
  }
}

/** The view on screen, or null before the shell exists. */
function activeView() {
  return shell ? views.get(shell.current()) : null;
}

/**
 * The pressed Run is the *active view's* Run. A diagnostics run compiles, a scene run lays out, a
 * canvas run draws; the page does not guess which one a visitor meant, because the view on screen is
 * the answer -- and it is the same single button the merge promised.
 */
function runActiveView() {
  const active = activeView();
  if (active && typeof active.run === 'function') {
    void active.run();
  }
}

function loadSampleForActiveView() {
  const active = activeView();
  if (active && typeof active.loadSample === 'function') {
    active.loadSample();
    return;
  }
  // A view that defines no sample of its own still gets a usable editor rather than a dead button.
  source.value = startupSample();
  source.focus();
}

/**
 * Builds one view with the page's shared services.
 *
 * `studio` is a *getter* rather than a value: the chrome mounts asynchronously and a view opened
 * before it resolves still has to read the toolbar once it does, so a snapshot here would freeze that
 * view at the default viewport.
 */
function viewOptions(id, onChange) {
  return {
    source,
    studio: () => studio,
    buttons: {
      loadSample: sampleButton,
      runInput: document.getElementById('run-input'),
    },
    setEnabled,
    viewport: () => {
      const { viewport, dpr } = viewportOf(studio);
      return { width: viewport.width, height: viewport.height, dpr };
    },
    file: () => (studio ? studio.file() : DEFAULT_FILE),
    onEnvironmentChange: (next) => onChange('onEnvironmentChange', next),
    onSample: (sample) => onChange('onSample', sample),
  };
}

/** Mounts (once) and returns the view module for `id`. */
async function mountView(id, onChange) {
  if (views.has(id)) {
    return views.get(id);
  }
  const load = VIEW_LOADERS[id];
  if (!load) {
    return null;
  }
  const module = await load();
  // `mountX` is the one exported factory in each view module; the name is derived from the id, so a
  // new view is one entry in `VIEWS` plus one module rather than a growing switch here.
  const factory = module['mount' + id.charAt(0).toUpperCase() + id.slice(1)];
  if (typeof factory !== 'function') {
    throw new Error('the ' + id + ' view module exports no mount function');
  }
  const view = factory(viewOptions(id, onChange));
  views.set(id, view);
  if (typeof view.load === 'function') {
    await view.load();
  }
  return view;
}

/**
 * Pushes a change to *every* mounted view.
 *
 * All of them, not only the visible one: a scene tab that is hidden when the toolbar moves would
 * otherwise show bounds from a viewport the toolbar no longer names the moment it is opened. The
 * handlers are cheap when they have nothing to redo -- the canvas redraws only with a live device,
 * the diagnostics view does nothing -- so this is not work per keystroke.
 */
function broadcast(name, ...args) {
  for (const view of views.values()) {
    if (typeof view[name] === 'function') {
      view[name](...args);
    }
  }
}

async function main() {
  source.value = startupSample();
  // Nothing is runnable until a view says its module loaded: a enabled button that silently does
  // nothing is worse than a disabled one.
  runButton.disabled = true;

  // The chrome mounts *before* the first view is shown, so a `?view=scene#p5=<390x844>` link lays the
  // scene out once, at 390x844 -- rather than at the default rectangle and then again at the shared
  // one. A chrome that fails to mount is reported and the page still shows a view.
  try {
    studio = await mountStudio({
      source,
      startup: startupSample(),
      onEnvironmentChange: (next) => broadcast('onEnvironmentChange', next),
      onSample: (sample) => broadcast('onSample', sample),
    });
  } catch (error) {
    studio = null;
    const note = document.getElementById('link-note');
    if (note) {
      note.hidden = false;
      note.textContent = 'The preview controls could not start: ' + String(error);
    }
  }

  shell = mountShell({
    mount: (id) => mountView(id, broadcast),
    onShow: (id) => {
      // "Visible" is what the canvas view needs: a hidden pane must not draw, and a pane that becomes
      // visible again must draw the frame it missed while hidden.
      for (const [candidate, view] of views) {
        if (typeof view.setVisible === 'function') {
          view.setVisible(candidate === id);
        }
      }
      // A view that is already mounted can be run; one whose module is still loading cannot. Its own
      // `load` enables the button when it is ready (through `setEnabled`).
      const view = views.get(id);
      runButton.disabled = !(view && typeof view.run === 'function');
    },
    onError: (id, error) => {
      // A view module that fails to import is that view's problem: it is reported in that pane's own
      // status line, and the other views keep working. The console carries the detail because a pane
      // whose module never loaded may have no status node to write into.
      const status = document.getElementById(id + '-status');
      if (status) {
        status.textContent = 'view unavailable';
        status.className = 'status warning';
      }
      console.error('the ' + id + ' view failed to load:', error);
    },
  });

  // The first view is the URL's `?view=` when it names one, else the default. The old pages' paths
  // never reach here: `scene.html`/`canvas.html` are redirect stubs that translate themselves into
  // `index.html?view=scene`/`?view=canvas` before this page loads.
  shell.show(resolveView(globalThis.location && globalThis.location.search, DEFAULT_VIEW), {
    // The view is already in the URL (or is the default, which the URL omits): rewriting it here would
    // add a history entry for a navigation the visitor did not make.
    updateUrl: false,
  });

  // ---- the shared editor and buttons ---------------------------------------------------------

  runButton.addEventListener('click', runActiveView);
  sampleButton.addEventListener('click', loadSampleForActiveView);
  source.addEventListener('keydown', (event) => {
    if ((event.ctrlKey || event.metaKey) && event.key === 'Enter') {
      event.preventDefault();
      runActiveView();
    }
  });

  // ---- the build id (D-S5: the deployed URL serves the current build) ---------------------------

  // `build-id.txt` is written by the publish job; a local checkout has none, and the field says so
  // rather than inventing one. One fetch for the page, where the three pages each had their own.
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
}

main();
