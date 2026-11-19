// SPDX-License-Identifier: MIT

// The studio's accent tokens (designer/web/src/styles/base.css): WCAG 2.x AA contrast for every semantic
// pair, in light and dark, and parity between the OS-preference block and the explicit `data-theme`
// override (a drift would make "Theme: Dark" look different from a dark OS). No browser, no dependency.
//
//   node scripts/ci/studio_accent_contrast.mjs

import { readFile } from 'node:fs/promises';

const css = await readFile('designer/web/src/styles/base.css', 'utf8');

/** The declarations of the first block that follows `opener` (a literal selector, braces balanced one deep). */
function block(opener) {
  const start = css.indexOf(opener);
  if (start < 0) throw new Error(`no block ${opener}`);
  const open = css.indexOf('{', start);
  const close = css.indexOf('}', open);
  const body = css.slice(open + 1, close);
  return Object.fromEntries([...body.matchAll(/--([a-z-]+):\s*(#[0-9a-fA-F]{6})\s*;/g)].map((m) => [m[1], m[2].toLowerCase()]));
}

const light = block('\n:root {');
const mediaDark = block('(prefers-color-scheme: dark) {\n  :root {');
const lightOverride = block(':root[data-theme="light"] {');
const darkOverride = block(':root[data-theme="dark"] {');

function luminance(hex) {
  const channel = (offset) => {
    const value = parseInt(hex.slice(offset, offset + 2), 16) / 255;
    return value <= 0.03928 ? value / 12.92 : ((value + 0.055) / 1.055) ** 2.4;
  };
  return 0.2126 * channel(1) + 0.7152 * channel(3) + 0.0722 * channel(5);
}
function contrast(a, b) {
  const [hi, lo] = [luminance(a), luminance(b)].sort((x, y) => y - x);
  return (hi + 0.05) / (lo + 0.05);
}

// [foreground, background, minimum, what]: 4.5 for text, 3 for non-text indicators (WCAG 1.4.11).
const PAIRS = [
  ['accent-text', 'accent', 4.5, 'primary action label'],
  ['accent-fg', 'pane-bg', 4.5, 'accent text on a pane'],
  ['accent-fg', 'bg', 4.5, 'accent text on the page'],
  ['focus-ring', 'pane-bg', 3, 'focus ring'],
  ['focus-ring', 'bg', 3, 'focus ring on the page'],
  ['tab-indicator', 'pane-bg', 3, 'active tab indicator'],
  ['selection-fg', 'selection-bg', 4.5, 'menu and palette selection'],
  ['status-fg', 'status-bg', 4.5, 'status bar'],
];

let failures = 0;
for (const [name, tokens] of [['light', light], ['dark', darkOverride]]) {
  for (const [fg, bg, minimum, what] of PAIRS) {
    if (!tokens[fg] || !tokens[bg]) {
      console.log(`FAIL ${name}: --${fg} or --${bg} is not defined`);
      failures += 1;
      continue;
    }
    const ratio = contrast(tokens[fg], tokens[bg]);
    const pass = ratio >= minimum;
    console.log(`${pass ? 'ok  ' : 'FAIL'} ${name} ${what}: ${ratio.toFixed(2)}:1 (>= ${minimum})`);
    if (!pass) failures += 1;
  }
}
for (const [label, a, b] of [['light', light, lightOverride], ['dark', mediaDark, darkOverride]]) {
  const drift = Object.keys({ ...a, ...b }).filter((key) => a[key] !== b[key]);
  console.log(`${drift.length === 0 ? 'ok  ' : 'FAIL'} ${label}: OS-preference tokens equal the data-theme override${drift.length ? ' -- ' + drift.join(', ') : ''}`);
  if (drift.length > 0) failures += 1;
}
if (failures > 0) {
  console.error(`${failures} accent token problem(s)`);
  process.exit(1);
}
console.log('accent tokens: ok');
