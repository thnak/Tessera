// SPDX-License-Identifier: MIT
//
// The editor tab strip's rules (#562): the replaceable preview tab, pinning, closing, and the path breadcrumbs.
// The model is imported from dist-ssr (`npm run build:chrome` is a prerequisite; a missing build is exit code 2).
//
//   node scripts/ci/preview_editor_tabs.mjs

import { dirname, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

import { assert, finish, test } from './mock_test_support.mjs';

const DIST = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..', 'designer', 'web', 'dist-ssr');
let model;
try {
  model = await import(pathToFileURL(resolve(DIST, 'editor-tabs.js')).href);
} catch {
  process.stderr.write('preview_editor_tabs: ' + DIST + ' is missing; run npm run build:chrome --prefix designer/web first\n');
  process.exit(2);
}
const { openPreview, pinTab, closeTabIn, breadcrumbs } = model;
const empty = { open: [], preview: null };

await test('the first single click opens a preview tab', () => {
  const t = openPreview(empty, 'a.tsx');
  assert(JSON.stringify(t) === JSON.stringify({ open: ['a.tsx'], preview: 'a.tsx' }), JSON.stringify(t));
});

await test('the next single click replaces the preview in place', () => {
  let t = { open: ['pinned.tsx', 'a.tsx', 'last.css'], preview: 'a.tsx' };
  t = openPreview(t, 'b.tsx');
  assert(JSON.stringify(t.open) === JSON.stringify(['pinned.tsx', 'b.tsx', 'last.css']), JSON.stringify(t));
  assert(t.preview === 'b.tsx', 'b is the preview');
});

await test('a pinned tab is never replaced; a new file is appended as the preview', () => {
  let t = pinTab(openPreview(empty, 'a.tsx'), 'a.tsx');
  assert(t.preview === null, 'pinned');
  t = openPreview(t, 'b.tsx');
  assert(JSON.stringify(t) === JSON.stringify({ open: ['a.tsx', 'b.tsx'], preview: 'b.tsx' }), JSON.stringify(t));
});

await test('opening an already open file changes nothing, and pinning another path is a no-op', () => {
  const t = { open: ['a.tsx', 'b.tsx'], preview: 'b.tsx' };
  assert(openPreview(t, 'a.tsx') === t, 'open file keeps its state');
  assert(pinTab(t, 'a.tsx') === t, 'other path');
});

await test('a dirty preview is not replaced', () => {
  const t = openPreview({ open: ['a.tsx'], preview: 'a.tsx' }, 'b.tsx', ['a.tsx']);
  assert(JSON.stringify(t.open) === JSON.stringify(['a.tsx', 'b.tsx']), JSON.stringify(t));
});

await test('closing the preview empties the slot; closing another tab keeps it', () => {
  const t = { open: ['a.tsx', 'b.tsx'], preview: 'b.tsx' };
  assert(closeTabIn(t, 'a.tsx').preview === 'b.tsx', 'kept');
  assert(closeTabIn(t, 'b.tsx').preview === null, 'cleared');
});

await test('breadcrumbs split a path into cumulative segments', () => {
  const c = breadcrumbs('ui/views/App.tsx');
  assert(c.map((x) => x.name).join('>') === 'ui>views>App.tsx', 'names');
  assert(c[1].path === 'ui/views' && c[2].file && !c[0].file, 'paths and file flag');
  assert(breadcrumbs('').length === 0, 'no path, no crumbs');
});

finish('preview_editor_tabs');
