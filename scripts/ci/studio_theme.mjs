// SPDX-License-Identifier: MIT

// The studio's theme editor core (issue #582): token read/write round trip that keeps comments and order, and
// WCAG contrast against known vectors and the shipped Instrument theme. No browser, no dependency.
//
//   node scripts/ci/studio_theme.mjs

import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { checkVariant, contrastRatio, floorFor } from '../../designer/web/theme-contrast.js';
import { formatOf, readTokens, setToken } from '../../designer/web/theme-tokens.js';

let failures = 0;
function check(name, fn) {
  try {
    fn();
    console.log('ok   ' + name);
  } catch (error) {
    failures += 1;
    console.log('FAIL ' + name + ': ' + error.message);
  }
}
const near = (actual, expected, tolerance = 0.01) => assert.ok(Math.abs(actual - expected) <= tolerance, actual + ' vs ' + expected);

// ---- contrast vectors ---------------------------------------------------------------------------------------
check('black on white is 21:1', () => near(contrastRatio('#000000', '#ffffff'), 21, 1e-9));
check('same color is 1:1', () => near(contrastRatio('#336699', '#336699'), 1, 1e-9));
check('#767676 on white is the AA boundary (4.54:1)', () => near(contrastRatio('#767676', '#ffffff'), 4.54));
check('#777777 on white just fails AA (4.48:1)', () => near(contrastRatio('#777777', '#ffffff'), 4.48));
check('short hex equals long hex', () => assert.equal(contrastRatio('#000', '#fff'), contrastRatio('#000000', '#ffffff')));
check('order does not matter', () => assert.equal(contrastRatio('#123456', '#fedcba'), contrastRatio('#fedcba', '#123456')));
check('a translucent foreground is judged over its background', () => near(contrastRatio('#00000080', '#ffffff'), 3.95, 0.1));
check('a non-hex color is unchecked, not guessed', () => assert.equal(contrastRatio('red', '#fff'), null));
check('floors: 4.5 text and 3 non-text, 7 and 4.5 in high contrast', () => {
  assert.deepEqual([floorFor('text', 'x-light'), floorFor('nontext', 'x-dark'), floorFor('large', 'x-light')], [4.5, 3, 3]);
  assert.deepEqual([floorFor('text', 'x-high-contrast'), floorFor('nontext', 'x-high-contrast')], [7, 4.5]);
});
check('a failing pair is reported with its floor', () => {
  const [row] = checkVariant({ '--on-surface': '#777777', '--surface': '#ffffff' }, 'p-light');
  assert.equal(row.pass, false);
  assert.equal(row.floor, 4.5);
});
check('high contrast raises the text floor to 7', () => {
  const tokens = { '--on-surface': '#666666', '--surface': '#ffffff' };
  assert.equal(checkVariant(tokens, 'p-light')[0].pass, true);
  assert.equal(checkVariant(tokens, 'p-high-contrast')[0].pass, false);
});

// ---- the shipped Instrument theme ---------------------------------------------------------------------------
const asset = await readFile('packages/tessera-ui/themes/instrument.theme.json', 'utf8');
check('the shipped Instrument theme passes its own table in all three variants', () => {
  const read = readTokens(asset, 'json');
  assert.ok(read.ok);
  assert.deepEqual(read.variants.map((variant) => variant.id), ['instrument-light', 'instrument-dark', 'instrument-high-contrast']);
  for (const variant of read.variants) {
    const tokens = Object.fromEntries(variant.tokens.map((token) => [token.name, token.value]));
    const rows = checkVariant(tokens, variant.id);
    assert.ok(rows.length > 20, variant.id + ' checks ' + rows.length + ' pairs');
    const failed = rows.filter((row) => row.pass !== true).map((row) => row.foreground + '/' + row.background + ' ' + row.ratio?.toFixed(2));
    assert.deepEqual(failed, [], variant.id);
  }
});

// ---- JSON round trip ----------------------------------------------------------------------------------------
check('JSON: formatOf', () => {
  assert.equal(formatOf('ui/instrument.theme.json'), 'json');
  assert.equal(formatOf('styles/theme.css'), 'css');
  assert.equal(formatOf('src/App.tsx'), null);
});
check('JSON: an edit changes only the value span; order and layout are byte-identical', () => {
  const result = setToken(asset, 'json', 'instrument-dark', '--accent', '#ff00aa');
  assert.ok(result.ok);
  assert.equal(result.previous, '#5aa2f0');
  assert.equal(result.text, asset.replace('"--accent": "#5aa2f0"', '"--accent": "#ff00aa"'));
  const back = setToken(result.text, 'json', 'instrument-dark', '--accent', '#5aa2f0');
  assert.equal(back.text, asset, 'writing the old value back restores the file exactly');
});
check('JSON: the same token in another variant is untouched', () => {
  const result = setToken(asset, 'json', 'instrument-dark', '--accent', '#ff00aa');
  const light = readTokens(result.text, 'json').variants[0].tokens.find((token) => token.name === '--accent');
  assert.equal(light.value, '#0b5cad');
});
check('JSON: refusals', () => {
  assert.equal(setToken(asset, 'json', 'instrument-dark', '--accent', 'not a color').ok, false);
  assert.equal(setToken(asset, 'json', 'instrument-dark', '--accent', '#fff;}').ok, false);
  assert.equal(setToken(asset, 'json', 'instrument-dark', '--nope', '#fff').ok, false);
  assert.equal(setToken(asset, 'json', 'nope', '--accent', '#fff').ok, false);
  assert.equal(setToken('{ "a": ', 'json', 'x', '--a', '#fff').ok, false);
});
check('JSON: a length token takes a length', () => {
  const result = setToken(asset, 'json', 'instrument-light', '--radius-md', '6px');
  assert.ok(result.ok);
  assert.ok(result.text.includes('"--radius-md": "6px"'));
});

// ---- CSS round trip -----------------------------------------------------------------------------------------
const css = [
  '/* Brand tokens: keep in sync with the design file. */',
  ':root {',
  '  --accent: #0b5cad; /* primary */',
  '  /* --old: #000000; */',
  '  --surface: #ffffff;',
  '  --radius-md: 4px;',
  '}',
  '',
  ':root[data-theme="dark"] {',
  '  --accent: #5aa2f0;',
  '  --surface: #101214; /* page */',
  '}',
  '',
  '@media (prefers-contrast: more) {',
  '  :root {',
  '    --accent: #003c8a;',
  '    --surface: #ffffff;',
  '  }',
  '}',
  '',
  '.button { color: var(--accent); }',
  '',
].join('\n');
check('CSS: variants come from the selector and the @media context', () => {
  const read = readTokens(css, 'css');
  assert.deepEqual(read.variants.map((variant) => variant.id), ['light', 'dark', 'high-contrast']);
  assert.deepEqual(read.variants[0].tokens.map((token) => token.name), ['--accent', '--surface', '--radius-md']);
  assert.equal(read.variants[2].tokens[0].value, '#003c8a');
});
check('CSS: an edit keeps comments, a commented-out declaration, and every other byte', () => {
  const result = setToken(css, 'css', 'dark', '--surface', '#000000');
  assert.ok(result.ok);
  assert.equal(result.text, css.replace('--surface: #101214; /* page */', '--surface: #000000; /* page */'));
  assert.ok(result.text.includes('/* --old: #000000; */'));
  assert.equal(setToken(result.text, 'css', 'dark', '--surface', '#101214').text, css);
});
check('CSS: the commented-out declaration is not a token', () => {
  assert.equal(readTokens(css, 'css').variants[0].tokens.some((token) => token.name === '--old'), false);
});

if (failures > 0) {
  console.error(failures + ' theme editor check(s) failed');
  process.exit(1);
}
console.log('studio theme: ok');
