// SPDX-License-Identifier: MIT
//
// Static checks for the P5 page wiring (issue #169): the things a stub-DOM test cannot see.
//
//   node scripts/ci/preview_page_checks.mjs
//
// Three checks, each for a failure this slice actually hit or would have shipped:
//
// 1. **Every element `studio.js` looks up exists in both pages.** `mountStudio` resolves its nodes by
//    id, so a page that drops one throws at load -- invisible to a stub, and invisible to
//    `node --check`.
// 2. **Every `samples.json` path exists.** The Pages lane copies `samples/` into the site and the
//    gallery fetches `./<path>`; a missing file is a button that cannot load. This passes locally
//    (where `samples/` sits beside `designer/`) and fails only on the deployed site, which is the
//    worst place to find it.
// 3. **No page calls a function it does not declare.** `app.js` kept a `void start()` after `start()`
//    was folded into `mountStudio`, and `scene.js` used a `file` its refactor had deleted; both throw
//    only when that line runs. The audit is deliberately shallow -- a scope-free identifier scan, not
//    a parser -- because it only has to catch "the name is gone".
//
// This is NOT a rendering test. That the pages paint is what the deploy and a human opening the URL
// verify; nothing here claims otherwise.

import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
const WEB = resolve(REPO_ROOT, 'designer', 'web');

const REQUIRED_IDS = [
  'controls',
  'share',
  'share-status',
  'toggle-gallery',
  'gallery',
  'gallery-list',
  'link-note',
  'source',
  'status',
];

// The scene page's *own* ids (P2/P4): `scene.js` and `scene-input.js` resolve them directly, without a
// shared module in between, so nothing else would catch one being renamed out from under them.
// `scene.html` merged P4's input pane and P5's toolbar without a conflict, which makes it the contract
// both halves of `scene.js` have to satisfy; this list is that contract written down.
const SCENE_IDS = [
  'source',
  'run',
  'load-sample',
  'status',
  'nodes',
  'empty-state',
  'build-id',
  'script',
  'run-input',
  'input-status',
  'cells',
];

// Browser and language globals a page may use without declaring. Kept explicit so the audit fails on
// an unknown name instead of silently accepting one.
const BROWSER_GLOBALS = new Set([
  'document', 'window', 'globalThis', 'console', 'fetch', 'navigator', 'location', 'history',
  'localStorage', 'JSON', 'Math', 'Number', 'String', 'Boolean', 'Array', 'Object', 'Promise',
  'Error', 'TypeError', 'Set', 'Map', 'Date', 'TextEncoder', 'TextDecoder', 'Blob', 'Response',
  'Uint8Array', 'CompressionStream', 'DecompressionStream', 'getComputedStyle', 'parseFloat',
  'parseInt', 'isNaN', 'Reflect', 'Proxy', 'undefined', 'null', 'true', 'false', 'this',
  'arguments', 'await', 'async', 'function', 'return', 'if', 'else', 'for', 'while', 'const', 'let',
  'var', 'new', 'typeof', 'instanceof', 'of', 'in', 'do', 'switch', 'case', 'break', 'continue',
  'try', 'catch', 'finally', 'throw', 'class', 'extends', 'super', 'import', 'export', 'default',
  'from', 'void', 'delete', 'yield', 'static', 'get', 'set',
]);

// Callback parameters a shallow scan cannot bind. `d` is the diagnostics page's `filter((d) => ...)`.
const KNOWN_CALLBACK_PARAMS = new Set(['d']);

let failures = 0;

function fail(message) {
  process.stdout.write(`FAIL ${message}\n`);
  failures += 1;
}

function ok(message) {
  process.stdout.write(`ok   ${message}\n`);
}

// --- 1. the pages carry the elements the shared chrome looks up ---------------------------------

for (const page of ['index.html', 'scene.html']) {
  const html = readFileSync(resolve(WEB, page), 'utf8');
  let missing = 0;
  for (const id of REQUIRED_IDS) {
    if (!html.includes(`id="${id}"`)) {
      fail(`${page} is missing id="${id}" that studio.js requires`);
      missing += 1;
    }
  }
  if (missing === 0) {
    ok(`${page} carries every element studio.js looks up`);
  }
}

// The scene page's own ids, on top of the shared chrome's: this is the P4 + P5 contract in one place,
// so a merge that kept one stage's DOM and dropped the other's fails here rather than in a browser.
{
  const html = readFileSync(resolve(WEB, 'scene.html'), 'utf8');
  let missing = 0;
  for (const id of SCENE_IDS) {
    if (!html.includes(`id="${id}"`)) {
      fail(`scene.html is missing id="${id}" that scene.js/scene-input.js requires`);
      missing += 1;
    }
  }
  if (missing === 0) {
    ok(`scene.html carries every id scene.js and scene-input.js look up (${SCENE_IDS.length})`);
  }
}

// --- 2. every manifest path exists, under the path the site uses ---------------------------------

const manifest = JSON.parse(readFileSync(resolve(WEB, 'samples.json'), 'utf8'));
if (manifest.schema !== 1) {
  fail(`samples.json is schema ${manifest.schema}, expected 1`);
}
let missingSamples = 0;
for (const entry of manifest.samples || []) {
  try {
    readFileSync(resolve(REPO_ROOT, entry.path));
  } catch {
    fail(`samples.json names a missing file: ${entry.path}`);
    missingSamples += 1;
  }
}
if (missingSamples === 0) {
  ok(`samples.json names ${(manifest.samples || []).length} sample(s), all present`);
}

// --- 3. no page calls a function it does not declare --------------------------------------------

/** Names read as bare identifiers that the file neither declares, imports, nor inherits. */
function undeclaredNames(text) {
  const declared = new Set();
  for (const match of text.matchAll(/\b(?:function|const|let|var|class)\s+([A-Za-z_$][\w$]*)/g)) {
    declared.add(match[1]);
  }
  for (const match of text.matchAll(/import\s*\{([^}]*)\}/g)) {
    for (const part of match[1].split(',')) {
      const name = part.trim().split(/\s+as\s+/).pop().trim();
      if (/^[A-Za-z_$][\w$]*$/.test(name)) {
        declared.add(name);
      }
    }
  }
  for (const match of text.matchAll(/import\s+([A-Za-z_$][\w$]*)\s+from/g)) {
    declared.add(match[1]);
  }
  for (const match of text.matchAll(/\{([^{}]*)\}\s*=/g)) {
    for (const part of match[1].split(',')) {
      const name = part.split(':').pop().trim();
      if (/^[A-Za-z_$][\w$]*$/.test(name)) {
        declared.add(name);
      }
    }
  }
  for (const match of text.matchAll(/\(([^()]*)\)\s*(?:=>|\{)/g)) {
    for (const part of match[1].split(',')) {
      const name = part.trim();
      if (/^[A-Za-z_$][\w$]*$/.test(name)) {
        declared.add(name);
      }
    }
  }
  for (const match of text.matchAll(/([A-Za-z_$][\w$]*)\s*=>/g)) {
    declared.add(match[1]);
  }

  // Strip comments and string literals before scanning, so a word inside either is not a reference.
  const source = text
    .replace(/\/\/[^\n]*/g, '')
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/`(?:\\.|[^`\\])*`/g, '""')
    .replace(/'(?:\\.|[^'\\])*'/g, '""')
    .replace(/"(?:\\.|[^"\\])*"/g, '""');

  const names = new Set();
  for (const match of source.matchAll(/(?<![.\w$])([A-Za-z_$][\w$]*)\s*(?=[({.[,;:)\]}?!<>=+\-*/%&|^~]|$)/g)) {
    const name = match[1];
    if (!declared.has(name) && !BROWSER_GLOBALS.has(name)) {
      names.add(name);
    }
  }
  return [...names].sort();
}

for (const page of ['app.js', 'scene.js', 'scene-input.js']) {
  const text = readFileSync(resolve(WEB, page), 'utf8');
  const real = undeclaredNames(text).filter(
    (name) => !KNOWN_CALLBACK_PARAMS.has(name) && text.includes(`${name}(`),
  );
  if (real.length > 0) {
    fail(`${page} calls undeclared function(s): ${real.join(', ')} (a leftover from a refactor?)`);
  } else {
    ok(`${page} calls no undeclared function`);
  }
}

process.stdout.write(
  failures === 0 ? '\npage checks: all passed\n' : `\npage checks: ${failures} failed\n`,
);
if (failures > 0) {
  process.exitCode = 1;
}
