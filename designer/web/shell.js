// SPDX-License-Identifier: MIT

// The merged studio page's shell: which view is showing, and the one navigation that changes it
// (issue #392, the merge of P1's diagnostics page, P2/P4's scene page and P3's canvas page into one
// `index.html`).
//
// **Why a shell module.** The three pages were three documents with three copies of a header, a
// toolbar and a `<main>`; the merge keeps one of each and three *views* inside it. This module is the
// only place that knows a view exists, so the URL parameter, the tab strip, the ARIA state and the
// "load this module lazily" decision all live in one file instead of three.
//
// **The view is part of the URL, not just of the DOM.** A share link or a bookmark that names a view
// has to arrive showing it: `?view=canvas` opens the canvas view, and `?view=scene#p5=...` decodes the
// fragment and then lays the scene out. The parameter is read here and written with
// `history.replaceState` on a tab click (replace, not push: a tab is not a navigation step a visitor
// wants to walk back through, and a back step that only flips a tab is a trap on a phone).
//
// **A view's module is imported on first use.** `VIEWS[].load` is a function returning the module's
// promise, and nothing here calls it at import time. That is the merge's central constraint: the
// diagnostics module stays on the first-paint path (it is the default view), the scene module loads
// when the Scene view is first shown, and the canvas module plus its WebGPU device load only when the
// Canvas view is first shown. This file itself is a few hundred bytes of navigation and imports no
// engine code, so a failed WebGPU import cannot take the snapshot view down
// (docs/design/reference-equantic-ui.md section 7.5).

import { parseViewport } from './preview-state.js';

/** The three views, in tab order. `hint` is what the page subtitle says while the view is open. */
export const VIEWS = Object.freeze([
  {
    id: 'diagnostics',
    label: 'Diagnostics',
    hint: 'Compiles your TSX with the same compiler the native build runs, in your browser.',
    editorHint:
      'Ctrl+Enter (or Cmd+Enter) runs. A diagnostics run has no viewport: the toolbar is carried ' +
      'into the share link, not applied to the compiler.',
  },
  {
    id: 'scene',
    label: 'Scene',
    hint: 'Evaluates your TSX with the same evaluator, style and layout engine, then lists the scene snapshot and the state cells a click script produces.',
    editorHint:
      'Ctrl+Enter (or Cmd+Enter) runs. The viewport and DPR are layout inputs here: the toolbar ' +
      're-runs the layout at the new size.',
  },
  {
    id: 'canvas',
    label: 'Canvas',
    hint: 'Paints the evaluated scene through WebGPU, on demand: the canvas redraws only when something actually changed.',
    editorHint:
      'Ctrl+Enter (or Cmd+Enter) runs. The canvas is sized from the toolbar and the device pixel ' +
      'ratio, and redraws only when something changed.',
  },
]);

/** The view a page without `?view=` shows. Diagnostics is the cheapest one and needs no GPU. */
export const DEFAULT_VIEW = 'diagnostics';

// The views this module has already handed to its caller. One entry per *page load*: a view's module
// is instantiated once, so flipping tabs does not re-fetch or re-instantiate a module, and a view that
// failed once is not retried on every tab click (the failure is on screen already).
const started = new Set();

/** The view id in a search string, or `null` when it names none of this page's views. */
function viewFromSearch(search) {
  const text = String(search ?? '');
  const query = text.startsWith('?') ? text.slice(1) : text;
  if (query === '') {
    return null;
  }
  let view = null;
  for (const part of query.split('&')) {
    const equals = part.indexOf('=');
    const key = equals < 0 ? part : part.slice(0, equals);
    if (key !== 'view') {
      continue;
    }
    const value = equals < 0 ? '' : decodeURIComponent(part.slice(equals + 1).replace(/\+/g, ' '));
    view = value;
  }
  return VIEWS.some((entry) => entry.id === view) ? view : null;
}

/**
 * The view the page should show: the URL's `?view=` when it names one, else the fragment's own
 * request, else `fallback`.
 *
 * The fragment carries no view field (`docs/specs/designer-studio-v1.md` section 7.1 is frozen: `v`,
 * `s`, `w`, `h`, `d`, `t`, `f` and nothing else), so a pre-merge `scene.html#p5=...` link can only
 * say "I was a scene link" through the *path* it arrived on. That is what the redirect stubs are for:
 * they translate the old path into `?view=`, and this function never has to guess.
 */
export function resolveView(search, fallback = DEFAULT_VIEW) {
  const named = viewFromSearch(search);
  if (named) {
    return named;
  }
  return VIEWS.some((entry) => entry.id === fallback) ? fallback : DEFAULT_VIEW;
}

/**
 * Wires the tab strip and the three view containers into a view switcher.
 *
 * `options.mount(viewId)` is called **once per view**, the first time that view is shown, and may
 * return a promise. A rejection is the view's own problem: it is reported through
 * `options.onError(viewId, error)` and the other views keep working.
 */
export function mountShell(options) {
  const tabs = new Map();
  const panels = new Map();
  for (const view of VIEWS) {
    const tab = document.getElementById('tab-' + view.id);
    const panel = document.getElementById('view-' + view.id);
    if (tab && panel) {
      tabs.set(view.id, tab);
      panels.set(view.id, panel);
    }
  }

  let current = null;

  function show(viewId, { updateUrl = true } = {}) {
    const id = VIEWS.some((view) => view.id === viewId) ? viewId : DEFAULT_VIEW;
    for (const [candidate, tab] of tabs) {
      const selected = candidate === id;
      tab.setAttribute('aria-selected', String(selected));
      // Roving tabindex: a tab strip is one tab stop, and the arrow keys move inside it.
      tab.tabIndex = selected ? 0 : -1;
    }
    for (const [candidate, panel] of panels) {
      panel.hidden = candidate !== id;
    }
    current = id;
    if (updateUrl) {
      writeViewToUrl(id, options.history, options.location);
    }
    const view = VIEWS.find((entry) => entry.id === id);
    if (view) {
      // Both hints are the page's: the subtitle describes the view, and the hint under the shared
      // editor says what Run will do to it. A view that wrote its own hint into the shared DOM would
      // leave it behind on the next tab.
      const subtitle = document.getElementById('page-hint');
      if (subtitle) {
        subtitle.textContent = view.hint;
      }
      const editorHint = document.getElementById('editor-hint');
      if (editorHint) {
        editorHint.textContent = view.editorHint;
      }
    }
    if (!started.has(id)) {
      started.add(id);
      // The import is deferred to here on purpose: `mount` is what pulls in the view's module and,
      // for canvas, what acquires a WebGPU device. Nothing below this line runs before the view is
      // first shown.
      Promise.resolve()
        .then(() => options.mount(id))
        .catch((error) => {
          if (options.onError) {
            options.onError(id, error);
          }
        });
    }
    if (options.onShow) {
      options.onShow(id);
    }
    return id;
  }

  // The tab strip's keyboard contract: Left/Right move between tabs and select, Home/End jump to the
  // ends. Without this a tab that is not current has `tabIndex = -1` and cannot be reached at all,
  // which is how a roving tabindex turns into an unreachable view.
  function onKeyDown(event) {
    const order = [...tabs.keys()];
    const index = order.indexOf(current);
    let next = null;
    if (event.key === 'ArrowRight') {
      next = order[(index + 1) % order.length];
    } else if (event.key === 'ArrowLeft') {
      next = order[(index - 1 + order.length) % order.length];
    } else if (event.key === 'Home') {
      next = order[0];
    } else if (event.key === 'End') {
      next = order[order.length - 1];
    }
    if (next) {
      event.preventDefault();
      show(next);
      const tab = tabs.get(next);
      if (tab && typeof tab.focus === 'function') {
        tab.focus();
      }
    }
  }

  for (const [id, tab] of tabs) {
    tab.addEventListener('click', () => show(id));
    tab.addEventListener('keydown', onKeyDown);
  }

  return {
    /** The view on screen. */
    current: () => current,
    /** Switches views, starting the view's module on first use. */
    show,
  };
}

/**
 * Puts `?view=<id>` in the address bar, preserving the query's other parameters and the fragment.
 *
 * `replaceState` rather than a plain assignment: a tab click must not push a history entry, and a
 * reload after one must still land on the same view. A `file:`/`data:` origin can refuse the rewrite;
 * the tabs still work, so the failure is swallowed rather than shown.
 */
function writeViewToUrl(viewId, history = globalThis.history, location = globalThis.location) {
  if (!history || typeof history.replaceState !== 'function' || !location) {
    return;
  }
  const params = new URLSearchParams(location.search || '');
  if (viewId === DEFAULT_VIEW) {
    // The default view costs no bytes in the address bar, so the plain site URL stays plain.
    params.delete('view');
  } else {
    params.set('view', viewId);
  }
  const query = params.toString();
  const url = `${location.pathname}${query ? '?' + query : ''}${location.hash || ''}`;
  try {
    history.replaceState(null, '', url);
  } catch {
    /* an origin that refuses the rewrite: the tabs still switch the view */
  }
}

/**
 * The viewport and DPR the toolbar currently selects, for a view that lays the source out.
 *
 * A view that has no toolbar yet (a direct call in a test, or a page whose chrome failed to mount)
 * gets the default rectangle rather than an exception: the scene view falling back to 320x240 is
 * strictly better than a page that throws because a `<select>` was missing.
 */
export function viewportOf(studio) {
  try {
    const controls = studio && typeof studio.controls === 'function' ? studio.controls() : null;
    if (!controls) {
      return { viewport: { width: 320, height: 240 }, dpr: 1 };
    }
    return { viewport: parseViewport(controls.viewport), dpr: Number(controls.dpr) };
  } catch {
    return { viewport: { width: 320, height: 240 }, dpr: 1 };
  }
}
