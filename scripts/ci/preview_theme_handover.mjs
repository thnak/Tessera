// SPDX-License-Identifier: MIT
//
// The page-side hand-over of the theme asset to the canvas module (issue #536).
//
//   node scripts/ci/preview_theme_handover.mjs
//
// Without the hand-over the module's `token` commit destination answers `not_token_bound`. This gate
// proves the two halves the page owns: `themeOf` picks the asset `tessera.json` `assets.theme` names
// with the theme the app opens in, and `handOverTheme` passes it through `tessera_canvas_set_theme`
// with the exact buffer text (the patch's offsets are measured in it). The engine half (what the
// module does with the asset) is `scene_canvas_patch_tests`. Needs Node with type stripping (v22.18+).

import assert from 'node:assert/strict';

import { themeOf } from '../../designer/web/src/lib/project-snapshot.ts';
import { handOverTheme } from '../../designer/web/src/lib/wasm-call.ts';

let failures = 0;
function check(name, body) {
  try {
    body();
    process.stdout.write(`ok   ${name}\n`);
  } catch (error) {
    failures += 1;
    process.stdout.write(`FAIL ${name}\n${error.stack}\n`);
  }
}

const THEME = '{"themes":{"default":{"--bg":"#fff"},"night":{"--bg":"#000"}},"start":"night"}';
const files = (extra = []) => [
  { path: 'tessera.json', name: 'tessera.json', kind: 'config', text: '', editable: true },
  { path: 'theme/theme.json', name: 'theme.json', kind: 'theme', text: '', editable: true },
  ...extra,
];
const reader = (texts) => (path) => texts[path] ?? '';

check('assets.theme names the asset and start names the theme', () => {
  const theme = themeOf(
    files(),
    reader({
      'tessera.json': '{"assets":{"theme":"theme/theme.json"}}',
      'theme/theme.json': THEME,
    }),
  );
  assert.deepEqual(theme, { path: 'theme/theme.json', text: THEME, themeId: 'night' });
});

check('the text is the buffer the reader returns, not a stored copy', () => {
  const edited = THEME.replace('#000', '#111');
  const theme = themeOf(
    files(),
    reader({ 'tessera.json': '{"assets":{"theme":"theme/theme.json"}}', 'theme/theme.json': edited }),
  );
  assert.equal(theme?.text, edited);
});

check('a start the asset does not define falls back to its first theme', () => {
  const text = '{"themes":{"alpha":{}},"start":"ghost"}';
  const theme = themeOf(
    files(),
    reader({ 'tessera.json': '{"assets":{"theme":"theme/theme.json"}}', 'theme/theme.json': text }),
  );
  assert.equal(theme?.themeId, 'alpha');
});

check('no assets.theme, a missing file or a non-theme document hand over nothing', () => {
  assert.equal(themeOf(files(), reader({ 'tessera.json': '{}' })), null);
  assert.equal(themeOf(files(), reader({ 'tessera.json': '{"assets":{"theme":"nope.json"}}' })), null);
  assert.equal(
    themeOf(
      files(),
      reader({ 'tessera.json': '{"assets":{"theme":"theme/theme.json"}}', 'theme/theme.json': '[]' }),
    ),
    null,
  );
  assert.equal(themeOf([], reader({})), null);
  assert.equal(themeOf(files(), reader({ 'tessera.json': '{not json' })), null);
});

// A fake Emscripten module: strings land in a table by pointer, so the call's arguments can be read.
function fakeModule(exported) {
  const heap = new Map();
  let next = 1;
  const module = {
    _malloc: () => next++,
    _free: (pointer) => heap.delete(pointer),
    _tessera_free: () => {},
    lengthBytesUTF8: (text) => Buffer.byteLength(text),
    stringToUTF8: (text, pointer) => heap.set(pointer, text),
    UTF8ToString: (pointer) => heap.get(pointer) ?? '{"ok":true}',
  };
  if (exported !== undefined) {
    module._tessera_canvas_set_theme = (...pointers) => {
      exported(pointers.map((pointer) => heap.get(pointer)));
      heap.set(1000, '{"ok":true}');
      return 1000;
    };
  }
  return module;
}

check('handOverTheme passes path, exact text and theme id', () => {
  let seen = null;
  const module = fakeModule((args) => (seen = args));
  assert.equal(handOverTheme(module, { path: 'theme/theme.json', text: THEME, themeId: 'night' }), true);
  assert.deepEqual(seen, ['theme/theme.json', THEME, 'night']);
});

check('handOverTheme(null) clears with empty strings', () => {
  let seen = null;
  const module = fakeModule((args) => (seen = args));
  handOverTheme(module, null);
  assert.deepEqual(seen, ['', '', '']);
});

check('a module without the export is left alone', () => {
  assert.equal(handOverTheme(fakeModule(), { path: 'a', text: 'b', themeId: 'c' }), false);
});

process.stdout.write(failures === 0 ? '\ntheme handover: all cases passed\n' : `\ntheme handover: ${failures} failed\n`);
if (failures > 0) {
  process.exitCode = 1;
}
