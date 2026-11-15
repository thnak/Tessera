// SPDX-License-Identifier: MIT

// The preview pages' shared UI: the sample gallery, the toolbar controls, the share link, and the
// local persistence of the toggles (issue #169, P5 of docs/design/reference-equantic-ui.md section
// 7.4).
//
// It is one module rather than three because the parts are one feature and they are only ever used
// together: the gallery needs the toolbar's viewport, the toolbar's state is what a link encodes, and
// the link's restore path sets the toolbar. What it is *not* is part of the first paint -- nothing
// here runs at import time. The manifest is fetched the first time the gallery is opened, and the
// share link is decoded only when the URL carries one, so `cold_engine_ready_ms` (P6) cannot grow
// because a page shipped a gallery.
//
// The gallery holds **no sample list of its own**: `samples.json` is generated from `samples/` by
// `scripts/ci/gen_sample_manifest.py` and carries a path and a component name. The TSX is fetched from
// that path at selection time, so the file in the repository stays the single source of truth and
// adding a sample never means editing JavaScript.

import {
  DEFAULT_FILE,
  THEMES,
  VIEWPORTS,
  DPRS,
  ShareError,
  parseViewport,
} from './preview-state.js';
import { decodeShare, encodeShare, stateFromHash } from './share.js';
import { mountGallery } from './gallery.js';

export { THEMES, VIEWPORTS, DPRS, parseViewport };
export { mountGallery, loadSampleManifest, loadSampleSource, titleOf, MANIFEST_SCHEMA } from './gallery.js';

// The locally stored toggles, kept apart from the fragment so a local toggle cannot rewrite the URL
// and a link can override what a visitor last chose here.
const STORAGE_KEY = 'tessera.preview.p5';

function safeStorage() {
  try {
    // Merely reading `localStorage` throws in some sandboxed frames, so probe it here.
    return globalThis.localStorage || null;
  } catch {
    return null;
  }
}

/** The locally stored toggles, or `null` when there are none (or storage is unavailable). */
export function readLocalPreferences(storage = safeStorage()) {
  if (!storage) {
    return null;
  }
  try {
    const text = storage.getItem(STORAGE_KEY);
    return text ? JSON.parse(text) : null;
  } catch {
    // A private-mode browser or a corrupted value: preferences are an optimization, not state.
    return null;
  }
}

/** Writes the toggles locally. Failure is silent on purpose: the fragment still carries them. */
export function writeLocalPreferences(preferences, storage = safeStorage()) {
  if (!storage) {
    return;
  }
  try {
    storage.setItem(STORAGE_KEY, JSON.stringify(preferences));
  } catch {
    /* quota or a blocked store: the URL is the durable copy */
  }
}

/**
 * Builds the toolbar's three controls into `container` and wires them to `onChange(state)`.
 *
 * Returns `set(state)` so a decoded link can push its values into the controls, and `state()` to read
 * them back. The three groups are the slice's toggles: viewport, DPR, theme.
 */
export function mountControls(container, initial, onChange) {
  const groups = {};
  let current = { ...initial };

  function group(label, name, values, format, key) {
    const field = document.createElement('div');
    field.className = 'control';
    const caption = document.createElement('span');
    caption.className = 'control-label';
    caption.textContent = label;
    const select = document.createElement('select');
    select.id = 'control-' + name;
    select.setAttribute('aria-label', label);
    for (const value of values) {
      const option = document.createElement('option');
      option.value = String(value);
      option.textContent = format(value);
      select.append(option);
    }
    select.addEventListener('change', () => {
      current = { ...current, [key]: select.value };
      onChange(current);
    });
    field.append(caption, select);
    container.append(field);
    groups[name] = { select, key };
    return select;
  }

  group(
    'Viewport',
    'viewport',
    VIEWPORTS.map(([width, height]) => `${width}x${height}`),
    (value) => {
      const match = VIEWPORTS.find(([width, height]) => `${width}x${height}` === value);
      return match ? match[2] : value;
    },
    'viewport',
  );
  group('DPR', 'dpr', DPRS, (value) => String(value) + '\u00d7', 'dpr');
  group('Theme', 'theme', THEMES, (value) => value, 'theme');

  function set(state) {
    // Every field is coerced to the string a `<select>` holds, so `state()` has one type whether or
    // not a visitor has touched the control yet. A number here and a string there would make every
    // caller remember which, which is exactly how a comparison silently stops matching.
    current = {
      ...current,
      ...state,
      viewport: state.viewport === undefined ? current.viewport : String(state.viewport),
      dpr: state.dpr === undefined ? current.dpr : String(state.dpr),
      theme: state.theme === undefined ? current.theme : String(state.theme),
    };
    const [width, height] = String(current.viewport || '320x240').split('x');
    groups.viewport.select.value = `${Number(width)}x${Number(height)}`;
    groups.dpr.select.value = String(current.dpr);
    groups.theme.select.value = String(current.theme);
  }

  set(initial);
  return { set, state: () => ({ ...current }) };
}

/**
 * The state the page should start from: the URL's fragment when it carries one, else the locally
 * stored toggles, else `fallback`.
 *
 * A fragment wins over local storage because a link is an explicit instruction from whoever shared it;
 * local storage is only the visitor's last toolbar position. A fragment that fails to decode throws
 * `ShareError`, and the caller shows it: a broken link must say so rather than load the default source
 * and look like the link never arrived.
 */
export async function initialState(fallback, hash = globalThis.location && globalThis.location.hash) {
  const shared = await stateFromHash(hash);
  if (shared) {
    return { ...shared, from: 'link' };
  }
  const stored = readLocalPreferences();
  if (stored && typeof stored === 'object') {
    return { ...fallback, ...pickPreferences(stored), source: fallback.source, from: 'local' };
  }
  return { ...fallback, from: 'default' };
}

// Only the environment fields are ever read back from local storage: the source comes from the sample
// (or the visitor's editor), never from a previous session, so opening the page is not a surprise.
const PREFERENCE_KEYS = ['viewport', 'dpr', 'theme', 'file'];

function pickPreferences(stored) {
  const out = {};
  for (const key of PREFERENCE_KEYS) {
    if (stored[key] !== undefined) {
      out[key] = stored[key];
    }
  }
  return out;
}

/** The state object for `encodeShare` from the toolbar's string-valued controls. */
export function shareStateOf(source, controls, file = DEFAULT_FILE) {
  const viewport = parseViewport(controls.viewport);
  return {
    source,
    file,
    width: viewport.width,
    height: viewport.height,
    dpr: Number(controls.dpr),
    theme: String(controls.theme),
  };
}

/**
 * Writes the toolbar's position to the address bar **without** a fragment, so the toggles survive a
 * reload but a copy of the URL is not mistaken for a share link.
 */
export function rememberLocally(controls) {
  writeLocalPreferences(pickPreferences(controls));
}

/**
 * Writes the share link to the address bar (`history.replaceState`, so it does not add a back step)
 * and returns the text a "Copy link" button should put on the clipboard.
 */
export async function shareToLocation(state, history = globalThis.history, location = globalThis.location) {
  const payload = await encodeShare(state);
  const url = `${location.origin}${location.pathname}${location.search}#p5=${payload}`;
  try {
    history.replaceState(null, '', url);
  } catch {
    // A `data:` or `file:` origin can refuse the rewrite; the returned text is still shareable.
  }
  return url;
}

/**
 * Wires the whole P5 chrome into a page: the toolbar, the gallery, the share button and the "your
 * link did not load" banner.
 *
 * Both pages need exactly this and differ only in what a change *does* (`onEnvironmentChange`,
 * `onSample`) and in how they show a transient message (`setStatus`). Keeping the wiring here rather
 * than in each page is what stops the two pages from drifting apart -- and it is fewer shipped bytes
 * than two copies of the same DOM code.
 *
 * `options.startup` is the page's built-in sample: a link with an empty source falls back to it,
 * because an empty editor on arrival reads as a broken page.
 */
export async function mountStudio(options) {
  const byId = (id) => document.getElementById(id);
  const source = options.source;
  const shareStatus = byId('share-status');
  const galleryToggle = byId('toggle-gallery');
  const galleryHost = byId('gallery');
  const linkNote = byId('link-note');

  let file = DEFAULT_FILE;
  let controls = null;

  function showLinkProblem(error) {
    linkNote.hidden = false;
    linkNote.textContent =
      error instanceof ShareError
        ? `This share link could not be opened (${error.code}): ${error.message}`
        : `This share link could not be opened: ${error}`;
  }

  function shareState() {
    return controls
      ? shareStateOf(source.value, controls.state(), file)
      : { source: source.value, file, width: 320, height: 240, dpr: 1, theme: 'system' };
  }

  function onControls(next) {
    rememberLocally(next);
    if (options.onEnvironmentChange) {
      options.onEnvironmentChange(next);
    }
  }

  async function copyLink() {
    shareStatus.textContent = 'encoding...';
    try {
      const url = await shareToLocation(shareState());
      if (navigator.clipboard && navigator.clipboard.writeText) {
        await navigator.clipboard.writeText(url);
        shareStatus.textContent = 'link copied';
      } else {
        // No clipboard permission (or no secure context). The address bar already carries the link,
        // and saying so is better than a button that silently does nothing.
        shareStatus.textContent = 'link is in the address bar';
      }
    } catch (error) {
      shareStatus.textContent = '';
      showLinkProblem(error);
    }
  }

  // One manifest per page load: opening the gallery twice must not refetch, and two `mountStudio`
  // calls in one test run are two page loads.
  const gallery = { samples: null };

  function toggleGallery() {
    // The *next* state is the negation of the current one. Reading `aria-expanded`, writing it back
    // unchanged and hiding on `!open` is the easy version of this and it never opens the gallery.
    const open = galleryToggle.getAttribute('aria-expanded') !== 'true';
    galleryToggle.setAttribute('aria-expanded', String(open));
    galleryHost.hidden = !open;
    if (!open) {
      return;
    }
    // Fetched on first open only: the manifest must not be on the first-paint path (P6 budget).
    void mountGallery(
      byId('gallery-list'),
      (sample) => {
        source.value = sample.text;
        file = sample.path;
        galleryToggle.setAttribute('aria-expanded', 'false');
        galleryHost.hidden = true;
        if (typeof source.focus === 'function') {
          source.focus();
        }
        if (options.onSample) {
          options.onSample(sample);
        }
      },
      globalThis.fetch,
      gallery,
    );
  }

  const base = {
    source: options.startup,
    file: DEFAULT_FILE,
    viewport: '320x240',
    dpr: 1,
    theme: 'system',
  };
  // The banner starts hidden in the HTML, and is set explicitly here too: a page mounted after a good
  // link must not inherit a stale "your link failed" from a previous mount.
  linkNote.hidden = true;
  try {
    const state = await initialState(base);
    source.value = state.source === '' ? options.startup : state.source;
    file = state.file || DEFAULT_FILE;
    controls = mountControls(
      byId('controls'),
      {
        viewport: `${state.width || 320}x${state.height || 240}`,
        dpr: state.dpr,
        theme: state.theme,
      },
      onControls,
    );
  } catch (error) {
    // A broken link still gets a usable page: the page's own sample is loaded and the banner says
    // what was wrong with the link. The alternative -- an empty editor -- reads as a broken page.
    source.value = options.startup;
    file = DEFAULT_FILE;
    controls = mountControls(
      byId('controls'),
      { viewport: '320x240', dpr: 1, theme: 'system' },
      onControls,
    );
    showLinkProblem(error);
  }

  byId('share').addEventListener('click', () => void copyLink());
  galleryToggle.addEventListener('click', toggleGallery);

  return {
    /** The toolbar's current values, as strings. */
    controls: () => (controls ? controls.state() : { viewport: '320x240', dpr: 1, theme: 'system' }),
    /** The source file name a share link carries (the sample's path, or the default). */
    file: () => file,
    /** The state a share link would encode right now. */
    shareState,
  };
}

export { decodeShare, encodeShare };
