// SPDX-License-Identifier: MIT

// The sample gallery of the preview pages (issue #169, P5 of docs/design/reference-equantic-ui.md
// section 7.4), split out of `studio.js` so neither module carries the other's weight.
//
// The gallery holds **no sample list of its own**: `samples.json` is generated from `samples/` by
// `scripts/ci/gen_sample_manifest.py` and carries a path and a component name. The TSX is fetched from
// that path at selection time, so the file in the repository stays the single source of truth and
// adding a sample never means editing JavaScript.
//
// Nothing here runs at import time and nothing here is on the first-paint path: the manifest is
// fetched the first time a visitor opens the gallery, so P6's `cold_engine_ready_ms` cannot grow
// because a page shipped a gallery.

/** The manifest schema this page reads; a newer one is a page change, not a data fix. */
export const MANIFEST_SCHEMA = 1;

/**
 * Fetches and validates the sample manifest. Called lazily (the gallery's first open), never at
 * module load.
 *
 * Throws an `Error` with a readable message rather than returning a partial list: a gallery that
 * silently shows three of four samples is worse than one that says it could not load.
 */
export async function loadSampleManifest(fetchImpl = globalThis.fetch) {
  const response = await fetchImpl('./samples.json', { cache: 'no-cache' });
  if (!response.ok) {
    throw new Error(`the sample manifest is unavailable (HTTP ${response.status})`);
  }
  const document = await response.json();
  if (!document || document.schema !== MANIFEST_SCHEMA) {
    throw new Error(
      `the sample manifest is schema ${document && document.schema}, expected ${MANIFEST_SCHEMA}`,
    );
  }
  if (!Array.isArray(document.samples) || document.samples.length === 0) {
    throw new Error('the sample manifest lists no samples');
  }
  const samples = [];
  for (const entry of document.samples) {
    if (!entry || typeof entry.path !== 'string' || typeof entry.component !== 'string') {
      throw new Error('the sample manifest has an entry without a path and component');
    }
    // The gallery fetches this path. An absolute path or `..` would make the manifest an injection
    // point; the generator never writes one, and a hand-edited manifest is not trusted.
    if (entry.path.startsWith('/') || entry.path.includes('..') || /^[a-z]+:/i.test(entry.path)) {
      throw new Error(`the sample manifest has an unsafe path: ${entry.path}`);
    }
    samples.push({ path: entry.path, component: entry.component });
  }
  return samples;
}

/** Fetches one sample's TSX: the sample is the source of truth, so it is read, never embedded. */
export async function loadSampleSource(path, fetchImpl = globalThis.fetch) {
  const response = await fetchImpl('./' + path, { cache: 'no-cache' });
  if (!response.ok) {
    throw new Error(`cannot read ${path} (HTTP ${response.status})`);
  }
  return response.text();
}

/**
 * A display title derived from the file stem (`SafeAreaPage.tsx` -> "Safe Area Page"), qualified by
 * the sample's directory so two same-named files stay distinguishable.
 *
 * Derived rather than stored: the manifest carries no prose, so a rename cannot leave a stale title.
 */
export function titleOf(path) {
  const parts = String(path).split('/');
  const file = parts[parts.length - 1] || String(path);
  const stem = file.replace(/\.tsx?$/i, '');
  const words = stem
    .replace(/[_-]+/g, ' ')
    .replace(/([a-z0-9])([A-Z])/g, '$1 $2')
    .trim();
  const title = words.replace(/\b\w/g, (character) => character.toUpperCase());
  const group = parts.length > 2 ? parts[parts.length - 2] : '';
  return group ? `${title} (${group})` : title;
}

/**
 * Renders the sample buttons into `container` and reports a choice through `onPick`.
 *
 * `onPick(sample)` receives `{ path, component, text }`. `cache` is the caller's per-page-load
 * manifest holder (`{ samples: null }`), so opening the gallery twice does not refetch it and a new
 * page load does. A failed fetch is rendered *in the container* rather than only logged.
 */
export async function mountGallery(
  container,
  onPick,
  fetchImpl = globalThis.fetch,
  cache = { samples: null },
) {
  container.replaceChildren();

  // One note node per gallery, appended only when there is something to say. Keeping it in the
  // container (rather than replacing the children on each message) is what lets a per-sample failure
  // report into a node that is still attached.
  const note = document.createElement('p');
  note.className = 'gallery-note';

  function report(message, isWarning) {
    note.textContent = message;
    note.className = 'gallery-note' + (isWarning ? ' warning' : '');
    if (container.children.indexOf(note) === -1) {
      container.append(note);
    }
  }

  if (!cache.samples) {
    try {
      cache.samples = await loadSampleManifest(fetchImpl);
    } catch (error) {
      report(String(error && error.message ? error.message : error), true);
      return;
    }
  }

  for (const sample of cache.samples) {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'gallery-item';
    button.dataset.path = sample.path;
    button.append(document.createTextNode(titleOf(sample.path)));
    button.append(document.createTextNode(' '));
    const name = document.createElement('code');
    name.textContent = sample.component;
    button.append(name);
    button.addEventListener('click', async () => {
      button.disabled = true;
      try {
        const text = await loadSampleSource(sample.path, fetchImpl);
        onPick({ ...sample, text });
      } catch (error) {
        report(String(error && error.message ? error.message : error), true);
      } finally {
        button.disabled = false;
      }
    });
    container.append(button);
  }
}
